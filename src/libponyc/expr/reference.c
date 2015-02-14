#include "reference.h"
#include "literal.h"
#include "postfix.h"
#include "call.h"
#include "../pass/expr.h"
#include "../pass/names.h"
#include "../pass/flatten.h"
#include "../type/subtype.h"
#include "../type/assemble.h"
#include "../type/alias.h"
#include "../type/viewpoint.h"
#include "../type/cap.h"
#include "../type/reify.h"
#include "../type/lookup.h"
#include <string.h>
#include <assert.h>

/**
 * Make sure the definition of something occurs before its use. This is for
 * both fields and local variable.
 */
static bool def_before_use(ast_t* def, ast_t* use, const char* name)
{
  if((ast_line(def) > ast_line(use)) ||
     ((ast_line(def) == ast_line(use)) &&
      (ast_pos(def) > ast_pos(use))))
  {
    ast_error(use, "declaration of '%s' appears after use", name);
    ast_error(def, "declaration of '%s' appears here", name);
    return false;
  }

  return true;
}

static bool is_assigned_to(ast_t* ast, bool check_result_needed)
{
  while(true)
  {
    ast_t* parent = ast_parent(ast);

    switch(ast_id(parent))
    {
      case TK_ASSIGN:
      {
        // Has to be the left hand side of an assignment. Left and right sides
        // are swapped, so we must be the second child.
        if(ast_childidx(parent, 1) != ast)
          return false;

        if(!check_result_needed)
          return true;

        // The result of that assignment can't be used.
        return !is_result_needed(parent);
      }

      case TK_SEQ:
      {
        // Might be in a tuple on the left hand side.
        if(ast_childcount(parent) > 1)
          return false;

        break;
      }

      case TK_TUPLE:
        break;

      default:
        return false;
    }

    ast = parent;
  }
}

static bool is_constructed_from(typecheck_t* t, ast_t* ast, ast_t* type)
{
  ast_t* parent = ast_parent(ast);

  if(ast_id(parent) != TK_DOT)
    return false;

  AST_GET_CHILDREN(parent, left, right);
  ast_t* find = lookup_try(t, parent, type, ast_name(right));

  if(find == NULL)
    return false;

  bool ok = ast_id(find) == TK_NEW;
  ast_free_unattached(find);
  return ok;
}

static bool valid_reference(typecheck_t* t, ast_t* ast, ast_t* type,
  sym_status_t status)
{
  if(is_constructed_from(t, ast, type))
    return true;

  switch(status)
  {
    case SYM_DEFINED:
      return true;

    case SYM_CONSUMED:
      if(is_assigned_to(ast, true))
        return true;

      ast_error(ast, "can't use a consumed local in an expression");
      return false;

    case SYM_UNDEFINED:
      if(is_assigned_to(ast, true))
        return true;

      ast_error(ast, "can't use an undefined variable in an expression");
      return false;

    default: {}
  }

  assert(0);
  return false;
}

bool expr_field(pass_opt_t* opt, ast_t* ast)
{
  AST_GET_CHILDREN(ast, id, type, init);

  if(ast_id(init) != TK_NONE)
  {
    // Initialiser type must match declared type.
    if(!coerce_literals(&init, type, opt))
      return false;

    ast_t* init_type = ast_type(init);

    if(is_typecheck_error(init_type))
      return false;

    init_type = alias(init_type);

    if(!is_subtype(init_type, type))
    {
      ast_error(init,
        "field/param initialiser is not a subtype of the field/param type");
      ast_error(type, "field/param type: %s", ast_print_type(type));
      ast_error(init, "initialiser type: %s", ast_print_type(init_type));
      ast_free_unattached(init_type);
      return false;
    }

    ast_free_unattached(init_type);
  }

  ast_settype(ast, type);
  return true;
}

bool expr_fieldref(typecheck_t* t, ast_t* ast, ast_t* find, token_id tid)
{
  AST_GET_CHILDREN(ast, left, right);
  ast_t* l_type = ast_type(left);

  if(is_typecheck_error(l_type))
    return false;

  AST_GET_CHILDREN(find, id, f_type, init);

  // Viewpoint adapted type of the field.
  ast_t* type = viewpoint_type(l_type, f_type);

  if(type == NULL)
  {
    ast_error(ast, "can't read a field from a tag");
    return false;
  }

  // Set the type so that it isn't free'd as unattached.
  ast_setid(ast, tid);
  ast_settype(ast, type);

  if(ast_id(left) == TK_THIS)
  {
    // Handle symbol status if the left side is 'this'.
    ast_t* id = ast_child(find);
    const char* name = ast_name(id);

    sym_status_t status;
    ast_get(ast, name, &status);

    if(!valid_reference(t, ast, type, status))
      return false;
  }

  return true;
}

bool expr_typeref(pass_opt_t* opt, ast_t** astp)
{
  ast_t* ast = *astp;
  assert(ast_id(ast) == TK_TYPEREF);
  ast_t* type = ast_type(ast);

  if(is_typecheck_error(type))
    return false;

  switch(ast_id(ast_parent(ast)))
  {
    case TK_QUALIFY:
      // Doesn't have to be valid yet.
      break;

    case TK_DOT:
      // Has to be valid.
      return expr_nominal(opt, &type);

    case TK_CALL:
    {
      // Has to be valid.
      if(!expr_nominal(opt, &type))
        return false;

      // Transform to a default constructor.
      ast_t* dot = ast_from(ast, TK_DOT);
      ast_add(dot, ast_from_string(ast, "create"));
      ast_swap(ast, dot);
      *astp = dot;
      ast_add(dot, ast);

      if(!expr_dot(opt, astp))
        return false;

      ast_t* ast = *astp;

      // If the default constructor has no parameters, transform to an apply
      // call.
      if(ast_id(ast) == TK_NEWREF)
      {
        type = ast_type(ast);

        if(is_typecheck_error(type))
          return false;

        assert(ast_id(type) == TK_FUNTYPE);
        AST_GET_CHILDREN(type, cap, typeparams, params, result);

        if(ast_id(params) == TK_NONE)
        {
          // Add a call node.
          ast_t* call = ast_from(ast, TK_CALL);
          ast_add(call, ast_from(call, TK_NONE)); // Named
          ast_add(call, ast_from(call, TK_NONE)); // Positional
          ast_swap(ast, call);
          ast_append(call, ast);

          if(!expr_call(opt, &call))
            return false;

          // Add a dot node.
          ast_t* apply = ast_from(call, TK_DOT);
          ast_add(apply, ast_from_string(call, "apply"));
          ast_swap(call, apply);
          ast_add(apply, call);

          if(!expr_dot(opt, &apply))
            return false;
        }
      }

      return true;
    }

    default:
    {
      // Has to be valid.
      if(!expr_nominal(opt, &type))
        return false;

      // Transform to a default constructor.
      ast_t* dot = ast_from(ast, TK_DOT);
      ast_add(dot, ast_from_string(ast, "create"));
      ast_swap(ast, dot);
      ast_add(dot, ast);

      // Call the default constructor with no arguments.
      ast_t* call = ast_from(ast, TK_CALL);
      ast_swap(dot, call);
      ast_add(call, dot); // Receiver comes last.
      ast_add(call, ast_from(ast, TK_NONE)); // Named args.
      ast_add(call, ast_from(ast, TK_NONE)); // Positional args.

      *astp = call;

      if(!expr_dot(opt, &dot))
        return false;

      return expr_call(opt, astp);
    }
  }

  return true;
}

bool expr_reference(pass_opt_t* opt, ast_t** astp)
{
  typecheck_t* t = &opt->check;
  ast_t* ast = *astp;

  // Everything we reference must be in scope.
  const char* name = ast_name(ast_child(ast));

  sym_status_t status;
  ast_t* def = ast_get(ast, name, &status);

  if(def == NULL)
  {
    ast_error(ast, "can't find declaration of '%s'", name);
    return false;
  }

  switch(ast_id(def))
  {
    case TK_PACKAGE:
    {
      // Only allowed if in a TK_DOT with a type.
      if(ast_id(ast_parent(ast)) != TK_DOT)
      {
        ast_error(ast, "a package can only appear as a prefix to a type");
        return false;
      }

      ast_setid(ast, TK_PACKAGEREF);
      return true;
    }

    case TK_INTERFACE:
    case TK_TRAIT:
    case TK_TYPE:
    case TK_TYPEPARAM:
    case TK_PRIMITIVE:
    case TK_CLASS:
    case TK_ACTOR:
    {
      // It's a type name. This may not be a valid type, since it may need
      // type arguments.
      ast_t* id = ast_child(def);
      const char* name = ast_name(id);
      ast_t* type = type_sugar(ast, NULL, name);
      ast_settype(ast, type);
      ast_setid(ast, TK_TYPEREF);

      return expr_typeref(opt, astp);
    }

    case TK_FVAR:
    case TK_FLET:
    {
      // Transform to "this.f".
      if(!def_before_use(def, ast, name))
        return false;

      ast_t* dot = ast_from(ast, TK_DOT);
      ast_add(dot, ast_child(ast));

      ast_t* self = ast_from(ast, TK_THIS);
      ast_add(dot, self);

      ast_replace(astp, dot);

      if(!expr_this(opt, self))
        return false;

      return expr_dot(opt, astp);
    }

    case TK_PARAM:
    {
      if(!def_before_use(def, ast, name))
        return false;

      ast_t* type = ast_type(def);

      if(is_typecheck_error(type))
        return false;

      if(!valid_reference(t, ast, type, status))
        return false;

      if(t->frame->def_arg != NULL)
      {
        ast_error(ast, "can't reference a parameter in a default argument");
        return false;
      }

      if(!sendable(type) && (t->frame->recover != NULL))
      {
        ast_error(ast,
          "can't access a non-sendable parameter from inside a recover "
          "expression");
        return false;
      }

      // Get the type of the parameter and attach it to our reference.
      // Automatically consume a parameter if the function is done.
      ast_t* r_type = type;

      if(is_method_return(t, ast))
        r_type = consume_type(type, TK_NONE);

      ast_settype(ast, r_type);
      ast_setid(ast, TK_PARAMREF);
      return true;
    }

    case TK_NEW:
    case TK_BE:
    case TK_FUN:
    {
      // Transform to "this.f".
      ast_t* dot = ast_from(ast, TK_DOT);
      ast_add(dot, ast_child(ast));

      ast_t* self = ast_from(ast, TK_THIS);
      ast_add(dot, self);

      ast_replace(astp, dot);

      if(!expr_this(opt, self))
        return false;

      return expr_dot(opt, astp);
    }

    case TK_ID:
    {
      if(!def_before_use(def, ast, name))
        return false;

      ast_t* type = ast_type(def);

      if(is_typecheck_error(type))
        return false;

      if(!valid_reference(t, ast, type, status))
        return false;

      ast_t* var = ast_parent(def);

      switch(ast_id(var))
      {
        case TK_VAR:
          ast_setid(ast, TK_VARREF);
          break;

        case TK_LET:
          ast_setid(ast, TK_LETREF);
          break;

        default:
          assert(0);
          return false;
      }

      if(!sendable(type))
      {
        if(t->frame->recover != NULL)
        {
          ast_t* def_recover = ast_nearest(def, TK_RECOVER);

          if(t->frame->recover != def_recover)
          {
            ast_error(ast, "can't access a non-sendable local defined outside "
              "of a recover expression from within that recover expression");
            return false;
          }
        }
      }

      // Get the type of the local and attach it to our reference.
      // Automatically consume a local if the function is done.
      ast_t* r_type = type;

      if(is_method_return(t, ast))
        r_type = consume_type(type, TK_NONE);

      ast_settype(ast, r_type);
      return true;
    }

    default: {}
  }

  assert(0);
  return false;
}

bool expr_local(typecheck_t* t, ast_t* ast)
{
  assert(t != NULL);
  assert(ast != NULL);

  AST_GET_CHILDREN(ast, id, type);
  assert(type != NULL);

  if(ast_id(type) == TK_NONE)
  {
    // No type specified, check we can infer
    if(!is_assigned_to(ast, false))
    {
      if(t->frame->pattern != NULL)
        ast_error(ast, "cannot infer type of capture variables");
      else
        ast_error(ast, "locals must specify a type or be assigned a value");

      return false;
    }

    type = ast_from(id, TK_INFERTYPE);
  }
  else if(ast_id(ast) == TK_LET && t->frame->pattern == NULL)
  {
    // Let, check we have a value assigned
    if(!is_assigned_to(ast, false))
    {
      ast_error(ast, "can't declare a let local without assigning to it");
      return false;
    }
  }

  ast_settype(id, type);
  ast_settype(ast, type);
  return true;
}

static bool expr_addressof_ffi(pass_opt_t* opt, ast_t* ast)
{
  ast_t* expr = ast_child(ast);

  switch(ast_id(expr))
  {
    case TK_FVARREF:
    case TK_VARREF:
    case TK_FLETREF:
    case TK_LETREF:
      break;

    case TK_PARAMREF:
      ast_error(ast, "can't take the address of a function parameter");
      return false;

    default:
      ast_error(ast, "can only take the address of a field or local variable");
      return false;
  }

  // Set the type to Pointer[ast_type(expr)].
  ast_t* expr_type = ast_type(expr);

  if(is_typecheck_error(expr_type))
    return false;

  ast_t* type = type_pointer_to(opt, expr_type);
  ast_settype(ast, type);
  return true;
}

bool expr_addressof(pass_opt_t* opt, ast_t* ast)
{
  // Check if we're in an FFI call.
  ast_t* parent = ast_parent(ast);

  if(ast_id(parent) == TK_SEQ)
  {
    parent = ast_parent(parent);

    if(ast_id(parent) == TK_POSITIONALARGS)
    {
      parent = ast_parent(parent);

      if(ast_id(parent) == TK_FFICALL)
        return expr_addressof_ffi(opt, ast);
    }
  }

  ast_t* expr = ast_child(ast);

  switch(ast_id(expr))
  {
    case TK_FVARREF:
    case TK_VARREF:
    case TK_FLETREF:
    case TK_LETREF:
    case TK_PARAMREF:
      break;

    default:
      ast_error(ast, "identity must be for a field, local or parameter");
      return false;
  }

  // Turn this into an identity operation. Set the type to U64.
  ast_t* type = type_builtin(opt, expr, "U64");
  ast_settype(ast, type);
  ast_setid(ast, TK_IDENTITY);
  return true;
}

bool expr_dontcare(ast_t* ast)
{
  // We are a tuple element. That tuple must either be a pattern or the LHS
  // of an assignment. It can be embedded in other tuples, which may appear
  // in sequences.
  ast_t* tuple = ast_parent(ast);
  assert(ast_id(tuple) == TK_TUPLE);

  ast_t* parent = ast_parent(tuple);

  while((ast_id(parent) == TK_TUPLE) || (ast_id(parent) == TK_SEQ))
  {
    tuple = parent;
    parent = ast_parent(tuple);
  }

  switch(ast_id(parent))
  {
    case TK_ASSIGN:
    {
      AST_GET_CHILDREN(parent, right, left);

      if(tuple == left)
      {
        ast_settype(ast, ast);
        return true;
      }

      break;
    }

    case TK_CASE:
    {
      AST_GET_CHILDREN(parent, pattern, guard, body);

      if(tuple == pattern)
      {
        ast_settype(ast, ast);
        return true;
      }

      break;
    }

    default: {}
  }

  ast_error(ast, "the don't care token can only appear in a tuple, either on "
    "the LHS of an assignment or in a pattern");
  return false;
}

bool expr_this(pass_opt_t* opt, ast_t* ast)
{
  // If this is the return value of a function, it is ephemeral.
  typecheck_t* t = &opt->check;
  token_id ephemeral;

  if(is_method_return(t, ast))
    ephemeral = TK_EPHEMERAL;
  else
    ephemeral = TK_NONE;

  token_id cap = cap_for_this(t);

  if(!cap_sendable(cap, ephemeral) && (t->frame->recover != NULL))
    cap = TK_TAG;

  ast_t* type = type_for_this(t, ast, cap, ephemeral);

  if(t->frame->def_arg != NULL)
  {
    ast_error(ast, "can't reference 'this' in a default argument");
    return false;
  }

  // Get the nominal type, which may be the right side of an arrow type.
  ast_t* nominal;
  bool arrow;

  if(ast_id(type) == TK_NOMINAL)
  {
    nominal = type;
    arrow = false;
  } else {
    nominal = ast_childidx(type, 1);
    arrow = true;
  }

  ast_t* typeargs = ast_childidx(nominal, 2);
  ast_t* typearg = ast_child(typeargs);

  while(typearg != NULL)
  {
    if(!expr_nominal(opt, &typearg))
    {
      ast_error(ast, "couldn't create a type for 'this'");
      ast_free(type);
      return false;
    }

    typearg = ast_sibling(typearg);
  }

  if(!expr_nominal(opt, &nominal))
  {
    ast_error(ast, "couldn't create a type for 'this'");
    ast_free(type);
    return false;
  }

  if(arrow)
    type = ast_parent(nominal);
  else
    type = nominal;

  ast_settype(ast, type);
  return true;
}

bool expr_tuple(ast_t* ast)
{
  ast_t* child = ast_child(ast);
  ast_t* type;

  if(ast_sibling(child) == NULL)
  {
    type = ast_type(child);
  } else {
    type = ast_from(ast, TK_TUPLETYPE);

    while(child != NULL)
    {
      ast_t* c_type = ast_type(child);

      if(c_type == NULL)
      {
        if(ast_id(child) == TK_DONTCARE)
        {
          c_type = child;
        } else {
          ast_error(child,
            "a tuple can't contain a control flow statement that never "
            "results in a value");
          return false;
        }
      }

      if(is_type_literal(c_type))
      {
        // At least one tuple member is literal, so whole tuple is
        ast_free(type);
        make_literal_type(ast);
        return true;
      }

      ast_append(type, c_type);
      child = ast_sibling(child);
    }
  }

  ast_settype(ast, type);
  ast_inheriterror(ast);
  return true;
}

bool expr_nominal(pass_opt_t* opt, ast_t** astp)
{
  // Resolve typealiases and typeparam references.
  if(!names_nominal(opt, *astp, astp))
    return false;

  ast_t* ast = *astp;

  switch(ast_id(ast))
  {
    case TK_TYPEPARAMREF:
      return flatten_typeparamref(ast) == AST_OK;

    case TK_NOMINAL:
      break;

    default:
      return true;
  }

  // If still nominal, check constraints.
  ast_t* def = (ast_t*)ast_data(ast);

  // Special case: don't check the constraint of a Pointer. This allows a
  // Pointer[Pointer[A]], which is normally not allowed, as a Pointer[A] is
  // not a subtype of Any.
  ast_t* id = ast_child(def);
  const char* name = ast_name(id);

  if(!strcmp(name, "Pointer"))
    return true;

  ast_t* typeparams = ast_childidx(def, 1);
  ast_t* typeargs = ast_childidx(ast, 2);

  return check_constraints(typeparams, typeargs, true);
}

static bool show_partiality(ast_t* ast)
{
  ast_t* child = ast_child(ast);
  bool found = false;

  while(child != NULL)
  {
    if(ast_canerror(child))
      found |= show_partiality(child);

    child = ast_sibling(child);
  }

  if(found)
    return true;

  if(ast_canerror(ast))
  {
    ast_error(ast, "an error can be raised here");
    return true;
  }

  return false;
}

static bool check_fields_defined(ast_t* ast)
{
  assert(ast_id(ast) == TK_NEW);

  ast_t* members = ast_parent(ast);
  ast_t* member = ast_child(members);
  bool result = true;

  while(member != NULL)
  {
    switch(ast_id(member))
    {
      case TK_FVAR:
      case TK_FLET:
      {
        sym_status_t status;
        ast_t* id = ast_child(member);
        ast_t* def = ast_get(ast, ast_name(id), &status);

        if((def != member) || (status != SYM_DEFINED))
        {
          ast_error(def, "field left undefined in constructor");
          result = false;
        }

        break;
      }

      default: {}
    }

    member = ast_sibling(member);
  }

  if(!result)
    ast_error(ast, "constructor with undefined fields is here");

  return result;
}

static bool check_return_type(ast_t* ast)
{
  assert(ast_id(ast) == TK_FUN);
  AST_GET_CHILDREN(ast, cap, id, typeparams, params, type, can_error, body);
  ast_t* body_type = ast_type(body);

  // The last statement is an error, and we've already checked any return
  // expressions in the method.
  if(body_type == NULL)
    return true;

  // If it's a compiler intrinsic, ignore it.
  if(ast_id(body_type) == TK_COMPILER_INTRINSIC)
    return true;

  // The body type must match the return type, without subsumption, or an alias
  // of the body type must be a subtype of the return type.
  ast_t* a_type = alias(type);
  ast_t* a_body_type = alias(body_type);
  bool ok = true;

  if(!is_subtype(body_type, type) || !is_subtype(a_body_type, a_type))
  {
    ast_t* last = ast_childlast(body);
    ast_error(last, "function body isn't the result type");
    ast_error(type, "function return type: %s", ast_print_type(type));
    ast_error(body_type, "function body type: %s", ast_print_type(body_type));
    ok = false;
  }

  ast_free_unattached(a_type);
  ast_free_unattached(a_body_type);
  return ok;
}

static bool check_main_create(typecheck_t* t, ast_t* ast)
{
  if(ast_id(t->frame->type) != TK_ACTOR)
    return true;

  ast_t* type_id = ast_child(t->frame->type);

  if(strcmp(ast_name(type_id), "Main"))
    return true;

  AST_GET_CHILDREN(ast, cap, id, typeparams, params, result, can_error);

  if(strcmp(ast_name(id), "create"))
    return true;

  bool ok = true;

  if(ast_id(typeparams) != TK_NONE)
  {
    ast_error(typeparams,
      "the create constructor of a Main actor must not be polymorphic");
    ok = false;
  }

  if(ast_childcount(params) != 1)
  {
    ast_error(params,
      "the create constructor of a Main actor must take a single Env "
      "parameter");
    ok = false;
  }

  ast_t* param = ast_child(params);

  if(param != NULL)
  {
    ast_t* p_type = ast_childidx(param, 1);

    if(!is_env(p_type))
    {
      ast_error(p_type, "must be of type Env");
      ok = false;
    }
  }

  return ok;
}

static bool check_finaliser(typecheck_t* t, ast_t* ast)
{
  if(ast_id(t->frame->type) != TK_ACTOR)
    return true;

  AST_GET_CHILDREN(ast, cap, id, typeparams, params, result, can_error);

  if(strcmp(ast_name(id), "_final"))
    return true;

  bool ok = true;

  if(ast_id(ast) != TK_FUN)
  {
    ast_error(ast, "actor _final must be a function");
    ok = false;
  }

  if(ast_id(cap) != TK_REF)
  {
    ast_error(cap, "actor _final must be ref");
    ok = false;
  }

  if(ast_id(typeparams) != TK_NONE)
  {
    ast_error(typeparams, "actor _final must not be polymorphic");
    ok = false;
  }

  if(ast_childcount(params) != 0)
  {
    ast_error(params, "actor _final must not have parameters");
    ok = false;
  }

  if(!is_none(result))
  {
    ast_error(result, "actor _final must return None");
    ok = false;
  }

  if(ast_id(can_error) != TK_NONE)
  {
    ast_error(can_error, "actor _final cannot raise an error");
    ok = false;
  }

  return ok;
}

bool expr_fun(pass_opt_t* opt, ast_t* ast)
{
  typecheck_t* t = &opt->check;

  AST_GET_CHILDREN(ast,
    cap, id, typeparams, params, type, can_error, body);

  if(ast_id(body) == TK_NONE)
    return true;

  if(!coerce_literals(&body, type, opt))
    return false;

  bool is_trait =
    (ast_id(t->frame->type) == TK_TRAIT) ||
    (ast_id(t->frame->type) == TK_INTERFACE);

  // Check partial functions.
  if(ast_id(can_error) == TK_QUESTION)
  {
    // If a partial function, check that we might actually error.
    if(!is_trait && !ast_canerror(body))
    {
      ast_error(can_error, "function body is not partial but the function is");
      return false;
    }
  } else {
    // If not a partial function, check that we can't error.
    if(ast_canerror(body))
    {
      ast_error(can_error, "function body is partial but the function is not");
      show_partiality(body);
      return false;
    }
  }

  if(!check_finaliser(t, ast))
    return false;

  switch(ast_id(ast))
  {
    case TK_NEW:
      return check_fields_defined(ast) && check_main_create(t, ast);

    case TK_FUN:
      return check_return_type(ast);

    default: {}
  }

  return true;
}

bool expr_compiler_intrinsic(typecheck_t* t, ast_t* ast)
{
  if(t->frame->method_body == NULL)
  {
    ast_error(ast, "a compiler intrinsic must be a method body");
    return false;
  }

  ast_t* child = ast_child(t->frame->method_body);

  // Allow a docstring before the compiler_instrinsic.
  if(ast_id(child) == TK_STRING)
    child = ast_sibling(child);

  if((child != ast) || (ast_sibling(child) != NULL))
  {
    ast_error(ast, "a compiler intrinsic must be the entire body");
    return false;
  }

  ast_settype(ast, ast_from(ast, TK_COMPILER_INTRINSIC));
  return true;
}

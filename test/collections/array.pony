class POINTER[A]

class Array[A] is Iterable[A]
  var size:U64
  var alloc:U64
  var ptr:POINTER[A]

  new create() =
    size = 0;
    alloc = 0;
    ptr = POINTER[A](0)

  fun length{iso|var|val}():U64 = size

  fun? apply{var|val}(i:U64):A->this =
    if i < size then ptr(i) else undef

  fun update{var}(i:U64, v:A):A =
    if i < size then ptr(i) = v else v

  fun reserve{iso|var}(len:U64) =
    if alloc < len then
      (alloc = len.max(8).next_pow2();
      ptr = POINTER[A].from(ptr, alloc))
    end

  fun clear{iso|var}() = size = 0

  fun append{var}(v:A) =
    reserve(size + 1);
    ptr(size) = v;
    size = size + 1

  fun concat{var}(iter:Iterable[A]) = for v in iter do append(v)

  fun iterator{var|val}():ArrayIterator[A, Array[A]{this}] =
    ArrayIterator(this)

class ArrayIterator[A, B:Array[A]] is Iterator[A]
  var array:B
  var i:U64

  new create(array':B) =
    array = array';
    i = 0

  fun has_next{iso|var|val}():Bool = i < array.length()

  fun next{var}():A->array = array(i = i + 1);

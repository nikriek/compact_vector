#ifndef __COMPACT_ITERATOR_H__
#define __COMPACT_ITERATOR_H__

#include <iterator>
#include <memory>
#include <type_traits>
#include <cstddef>
#include <climits>

#include "const_iterator_traits.hpp"
#include "parallel_iterator_traits.hpp"
#include "prefetch_iterator_traits.hpp"


namespace compact {
// Number of bits in type t
template<typename T>
struct bitsof {
  static constexpr size_t val = sizeof(T) * CHAR_BIT;
};

// Compact iterator definition. A 'iterator<int> p' would
// behave identically to an 'int*', except that the underlying storage
// is bit-packed. The actual number of bits used by each element is
// specified at construction.
//
// * IDX is the type of the integral type, i.e., the type of the
//   integer pointed to, as seen from the outside. It behaves like a
//   pointer to IDX.
//
// * BITS is the number of bits used for each word. If BITS==0, the
// * class is specialized to use a number of bits defined at runtime
// * instead of compile time.
//
// * W is the word type used internally. We must have sizeof(IDX) <=
//   sizeof(W).
//
// * TS is true for Thread Safe operations. It is only concerned with
//   basic thread safety: if p1 != p2, than manipulating *p1 and *p2
//   in 2 different thread is safe.
//
// * UB is the number of Used Bits in each word. We must have UB <=
//   bitsof<W>::val. Normally UB == bitsof<W>::val, but for some applications,
//   saving a few bits in each word can be useful (for example to
//   provide the compare and swap operation CAS).
template<typename IDX, unsigned BITS = 0, typename W = uint64_t,
         unsigned UB = bitsof<W>::val>
class const_iterator;
template<typename IDX, unsigned BITS = 0, typename W = uint64_t,
         bool TS = false, unsigned UB = bitsof<W>::val>
class iterator;

namespace iterator_imp {
template<typename IDX, unsigned BITS, typename W, unsigned UB>
struct getter;

template<typename IDX, typename W, unsigned UB>
struct getter<IDX, 0, W, UB> {
  static IDX get(const W* p, unsigned b, unsigned o) {
    static constexpr size_t Wbits  = bitsof<W>::val;
    static constexpr W      ubmask = ~(W)0 >> (Wbits - UB);
    W                       mask   = ((~(W)0 >> (Wbits - b)) << o) & ubmask;
    IDX                     res    = (*p & mask) >> o;
    if(o + b > UB) {
      const unsigned over  = o + b - UB;
      mask                     = ~(W)0 >> (Wbits - over);
      res                     |= (*(p + 1) & mask) << (b - over);
    }
    if(std::is_signed<IDX>::value && res & ((IDX)1 << (b - 1)))
      res |= ~(IDX)0 << b;

    return res;
  }
};

template<typename W, bool TS>
struct mask_store { };

template<typename W>
struct mask_store<W, false> {
  static inline void store(W* p, W mask, W val) {
    *p = (*p & ~mask) | (val & mask);
  }
};

template<typename W>
struct mask_store<W, true> {
  static void store(W* p, W mask, W val) {
    W cval = *p, oval;
    do {
      W nval = (cval & ~mask) | (val & mask);
      oval = cval;
      cval = __sync_val_compare_and_swap(p, oval, nval);
    } while(cval != oval);
  }

  // Do a CAS at p and val, only in the bits covered by mask. It
  // retries while bits outside of mask change but those inside the
  // mask are equal to the expected value exp.
  static bool cas(W* p, W mask, W val, W exp) {
    W cval = *p, oval;
    val &= mask;
    exp &= mask;
    if(val == exp)
      return (cval & mask) == exp;
    while((cval & mask) == exp) {
      W nval = (cval & ~mask) | val;
      oval = cval;
      cval = __sync_val_compare_and_swap(p, oval, nval);
      if(cval == oval)
        return true;
    }
    return false;
  }
};

template<typename IDX, typename W, bool TS = false, unsigned UB = bitsof<W>::val>
static void set(IDX x, W* p, unsigned b, unsigned o) {
  static constexpr size_t Wbits  = bitsof<W>::val;
  static constexpr W      ubmask = ~(W)0 >> (Wbits - UB);
  const W                 y      = x;
  W                       mask   = ((~(W)0 >> (Wbits - b)) << o) & ubmask;
  mask_store<W, TS>::store(p, mask, y << o);
  if(o + b > UB) {
    unsigned over = o + b - UB;
    mask              = ~(W)0 >> (Wbits - over);
    mask_store<W, TS>::store(p + 1, mask, y >> (b - over));
  }
}

// Do a CAS at position p, offset o and number of bits b. Expect value
// exp and set value x. It takes care of the tricky case when the
// value pointed by (p, o, b) straddles 2 words. Then it require 2 CAS
// and it is technically not lock free anymore: if the current thread
// dies after setting the MSB to 1 during the first CAS, then the
// location is "dead" and other threads maybe prevented from making
// progress.
template<typename IDX, typename W, unsigned UB>
static bool cas(const IDX x, const IDX exp, W* p, unsigned b, unsigned o) {
  static_assert(UB < bitsof<W>::val, "The CAS operation is valid for a cas_vector (used bits less than bits in word)");
  static constexpr size_t Wbits  = bitsof<W>::val;
  static constexpr W      ubmask = ~(W)0 >> (Wbits - UB);
  static_assert(UB < Wbits, "Used bits must strictly less than bit size of W for CAS.");
  if(o + b <= UB) {
    const W mask  = (~(W)0 >> (Wbits - b)) << o & ubmask;
    return mask_store<W, true>::cas(p, mask, (W)x << o, (W)exp << o);
  }

  // o + b > UB. Needs to do a CAS with MSB set to 1, expecting MSB at
  // 0. If failure, then return failure. If success, cas rest of value
  // in next word, then set MSB back to 0.
  static constexpr W msb = (W)1 << (Wbits - 1);
  W mask = (~(W)0 >> (Wbits - b)) << o;
  if(!mask_store<W, true>::cas(p, mask, msb | ((W)x << o), ~msb & ((W)exp << o)))
    return false;
  const unsigned over = o + b - UB;
  mask                    = ~(W)0 >> (Wbits - over);
  const bool         res  = mask_store<W, true>::cas(p + 1, mask, (W)x >> (b - over), (W)exp >> (b - over));
  mask_store<W, true>::store(p, msb, 0);
  return res;
}

// Fetch a value at position p, offset o and number of bits b. This is
// used when multiple thread may update the same position (p,o,b) with
// cas operations. In the case where the value straddles two words,
// then a CAS operation set the MSB to 1 (to prevent any other thread
// from changing the value), then reads the second words, finally sets
// the MSB back to 0 with a CAS operation.
//
// Result returned in res. Returns true if fetch is successfull
// (either value contained within a word, or CAS operations
// succeeded). Otherwise, it returns false.
template<typename IDX, typename W, unsigned UB>
static bool fetch(IDX& res, W* p, unsigned b, unsigned o) {
  static_assert(UB < bitsof<W>::val, "The fetch operation is valid for cas_vector (used bits less than bits in word");
  if(o + b <= UB) {
    res = get(p, b, o);
    return true;
  }

  // o + b > UB. Needs to do a CAS with MSB set to 1, expecting MSB at
  // 0. If failure, then return failure. If success, read entire value
  // then set MSB back to 0.
  static constexpr size_t Wbits  = bitsof<W>::val;
  static constexpr W      ubmask = ~(W)0 >> (Wbits - UB);
  static constexpr W      msb    = (W)1 << (Wbits - 1);
  const W                 mask   = (~(W)0 >> (Wbits - b)) << o;
  W                       x      = (*p & mask);
  if(x & msb) return false; // MSB already set to 1
  if(!mask_store<W, true>::cas(p, mask, msb | x, x))
    return false;
  const unsigned over  = o + b - UB;
  const W nmask            = ~(W)0 >> (Wbits - over);
  res                      = x | ((*(p + 1) & nmask) << (b - over));
  if(std::is_signed<IDX>::value && res & ((IDX)1 << (b - 1)))
    res |= ~(IDX)0 << b;
  mask_store<W, true>cas(p, mask, x, x | msb);
  return true;
}

template<class Derived, typename IDX, unsigned BITS, typename W, unsigned UB>
class common {
public:
  std::ostream& print(std::ostream& os) const {
    const Derived& self = *static_cast<const Derived*>(this);
    return os << '<' << (void*)self.ptr << '+' << self.offset << ',' << self.bits << '>';
  }

protected:
  static constexpr unsigned Wbits = bitsof<W>::val;
  // UB is the number of bits actually used in a word.
  static_assert(UB <= Wbits,
                "Number of used bits must be less than number of bits in a word");
  static_assert(sizeof(IDX) <= sizeof(W),
                "The size of integral type IDX must be less than the word type W");

public:
  typedef typename std::iterator<std::random_access_iterator_tag, IDX>::difference_type difference_type;
  static constexpr unsigned used_bits = UB;

  Derived& operator=(const Derived& rhs) {
    Derived& self = *static_cast<Derived*>(this);
    self.ptr      = rhs.ptr;
    self.offset   = rhs.offset;
    self.bits(rhs.bits());
    return self;
  }

  Derived& operator=(std::nullptr_t p) {
    Derived& self = *static_cast<Derived*>(this);
    self.ptr      = nullptr;
    self.offset   = 0;
  }

  IDX operator*() const {
    const Derived& self = *static_cast<const Derived*>(this);
    return getter<IDX, BITS, W, UB>::get(self.m_ptr, self.bits(), self.m_offset);
  }

  bool operator==(const Derived& rhs) const {
    const Derived& self = *static_cast<const Derived*>(this);
    return self.m_ptr == rhs.m_ptr && self.m_offset == rhs.m_offset;
  }
  bool operator!=(const Derived& rhs) const {
    return !(*this == rhs);
  }

  bool operator==(std::nullptr_t p) {
    const Derived& self = *static_cast<const Derived*>(this);
    return self.m_ptr == nullptr && self.m_offset == 0;
  }
  bool operator!=(std::nullptr_t p) {
    return !(*this == nullptr);
  }

  bool operator<(const Derived& rhs) const {
    const Derived& self = *static_cast<const Derived*>(this);
    return self.m_ptr < rhs.m_ptr || (self.m_ptr == rhs.m_ptr && self.m_offset < rhs.m_offset);
  }
  bool operator>(const Derived& rhs) const {
    const Derived& self = *static_cast<const Derived*>(this);
    return self.m_ptr > rhs.m_ptr || (self.m_ptr == rhs.m_ptr && self.m_offset > rhs.m_offset);
  }
  bool operator>=(const Derived& rhs) const {
    return !(*this < rhs);
  }
  bool operator<=(const Derived& rhs) const {
    return !(*this > rhs);
  }

  Derived& operator++() {
    Derived& self = *static_cast<Derived*>(this);
    self.m_offset += self.bits();
    if(self.m_offset >= UB) {
      ++self.m_ptr;
      self.m_offset -= UB;
    }
    return self;
  }
  Derived operator++(int) {
    Derived res(*static_cast<Derived*>(this));
    ++*this;
    return res;
  }

  Derived& operator--() {
    Derived& self = *static_cast<Derived*>(this);
    if(self.bits() > self.m_offset) {
      --self.m_ptr;
      self.m_offset += UB;
    }
    self.m_offset -= self.bits();
    return self;
  }
  Derived operator--(int) {
    Derived res(*static_cast<Derived*>(this));
    --*this;
    return res;
  }

  Derived& operator+=(difference_type n) {
    Derived&     self    = *static_cast<Derived*>(this);
    if(n < 0) {
      self -= -n;
      return self;
    }

    const size_t nbbits  = self.bits() * n;
    self.m_ptr          += nbbits / UB;
    self.m_offset       += nbbits % UB;
    if(self.m_offset >= UB) {
      ++self.m_ptr;
      self.m_offset -= UB;
    }
    return self;
  }

  Derived operator+(difference_type n) const {
    Derived res(*static_cast<const Derived*>(this));
    return res += n;
  }

  Derived& operator-=(difference_type n) {
    Derived&           self     = *static_cast<Derived*>(this);
    if(n < 0) {
      self += -n;
      return self;
    }

    const size_t      nbbits    = self.bits() * n;
    self.m_ptr                 -= nbbits / UB;
    const unsigned ooffset  = nbbits % UB;
    if(ooffset > self.m_offset) {
      --self.m_ptr;
      self.m_offset += UB;
    }
    self.m_offset -= ooffset;
    return self;
  }

  Derived operator-(difference_type n) const {
    Derived res(*static_cast<const Derived*>(this));
    return res -= n;
  }

  template<unsigned BB, typename DD>
  difference_type operator-(const common<DD, IDX, BB, W, UB>& rhs_) const {
    const Derived& self  = *static_cast<const Derived*>(this);
    const DD&      rhs   = *static_cast<const DD*>(&rhs_);
    ptrdiff_t      wdiff = (self.m_ptr - rhs.m_ptr) * UB;
    if(self.m_offset < rhs.m_offset)
      wdiff += (ptrdiff_t)((UB + self.m_offset) - rhs.m_offset) - (ptrdiff_t)UB;
    else
      wdiff += self.m_offset - rhs.m_offset;
    return wdiff / self.bits();
  }

  IDX operator[](const difference_type n) const {
    const Derived& self  = *static_cast<const Derived*>(this);
    return *(self + n);
  }

  // Extra methods which are not part of an iterator interface

  const W* get_ptr() const {
    const Derived& self  = *static_cast<const Derived*>(this);
    return self.ptr;
  }
  unsigned get_offset() const {
    const Derived& self  = *static_cast<const Derived*>(this);
    return self.offset;
  }
  unsigned get_bits() const {
    const Derived& self  = *static_cast<const Derived*>(this);
    return self.bits();
  }

  // Get some number of bits
  W get_bits(unsigned bits) const {
    const Derived& self  = *static_cast<const Derived*>(this);
    return getter<W, BITS, W, UB>::get(self.ptr, bits, self.offset);
  }

  W get_bits(unsigned bits, unsigned offset) const {
    const Derived& self  = *static_cast<const Derived*>(this);
    return getter<W, BITS, W, UB>::get(self.ptr, bits, offset);
  }

  template<bool TS = false>
  void set_bits(W x, unsigned bits) {
    Derived& self  = *static_cast<Derived*>(this);
    set<W, W, TS, UB>(x, self.ptr, bits, self.offset);
  }
};

template<typename W, int I = sizeof(W)>
struct swap_word_mask {
  static constexpr W value = swap_word_mask<W, I / 2>::value << (4 * I) | swap_word_mask<W, I / 2>::value;
};
template<typename W>
struct swap_word_mask<W, 1> {
  static constexpr W value = 0x55;
};

template<typename W>
inline W swap_word(W w) {
  return ((w & swap_word_mask<W>::value) << 1) | ((w & (swap_word_mask<W>::value << 1)) >> 1);
}
template<typename W>
inline bool compare_swap_words(W w1, W w2) {
  w1 = swap_word(w1);
  w2 = swap_word(w2);
  W bmask = w1 ^ w2;
  bmask &= -bmask;
  return (w1 & bmask) == 0;
}

// Precompute (expensive) division by number of bits. The static
// arrays contain X/k (word_idx) and k*(X/k)
// (word_bits) for k in [0, X].
//
// ** This code is kind of sick!

//helper template, just converts its variadic arguments to array initializer list
template<size_t... values> struct size_t_ary {static const size_t value[sizeof...(values)];};
template<size_t... values> const size_t size_t_ary<values...>::value[] = {values...};

template<size_t X, size_t k = X, size_t... values>
struct word_idx : word_idx <X, k-1, X / k, values...> {};
template<size_t X, size_t... values>
struct word_idx<X, 0, values...> : size_t_ary<(size_t)0, values...> {};

template<size_t X, size_t k = X, size_t... values>
struct word_bits : word_bits <X, k-1, k * (X / k), values...> {};
template<size_t X, size_t... values>
struct word_bits<X, 0, values...> : size_t_ary<(size_t)0, values...> {};

template<typename Iterator>
bool lexicographical_compare_n(Iterator first1, const size_t len1,
                               Iterator first2, const size_t len2) {
  static constexpr unsigned UB = Iterator::used_bits;

  const auto bits            = first1.get_bits();
  auto       left            = std::min(len1, len2) * bits;
  const decltype(len1) Widx  = word_idx<UB>::value[bits];
  const decltype(len1) Wbits = word_bits<UB>::value[bits];

  for( ; left > Wbits; left -= Wbits, first1 += Widx, first2 += Widx) {
    auto w1   = first1.get_bits(Wbits);
    auto w2   = first2.get_bits(Wbits);
    if(w1 != w2) return compare_swap_words(w1, w2);
  }
  if(left > 0) {
    auto w1   = first1.get_bits(left);
    auto w2   = first2.get_bits(left);
    if(w1 != w2) return compare_swap_words(w1, w2);
  }

  return len1 < len2;
}

template<typename D, typename I, unsigned B, typename W, unsigned U>
bool operator==(std::nullptr_t lfs, const common<D, I, B, W, U>& rhs) {
  return rhs == nullptr;
}

template<typename D, typename I, unsigned B, typename W, unsigned U>
D operator+(typename common<D, I, B, W, U>::difference_type lhs, const common<D, I, B, W, U>& rhs) {
  return rhs + lhs;
}

template<typename D, typename I, unsigned B, typename W, unsigned U>
std::ostream& operator<<(std::ostream& os, const common<D, I, B, W, U>& rhs) {
  return rhs.print(os);
}

template<typename IDX, unsigned BITS, typename W, bool TS = false, unsigned UB = bitsof<W>::val>
class setter;

template<typename IDX, typename W, bool TS, unsigned UB>
class setter<IDX, 0, W, TS, UB> {
  W*           ptr;
  unsigned bits;            // number of bits in an integral type
  unsigned offset;

  typedef compact::iterator<IDX, 0, W, TS, UB> iterator;
public:
  setter(W* p, int b, int o) : ptr(p), bits(b), offset(o) { }
  operator IDX() const { return getter<IDX, 0, W, UB>::get(ptr, bits, offset); }
  setter& operator=(const IDX x) {
    set<IDX, W, TS, UB>(x, ptr, bits, offset);
    return *this;
  }
  setter& operator=(const setter& rhs) {
    set<IDX, W, TS, UB>((IDX)rhs, ptr, bits, offset);
    return *this;
  }
  iterator operator&() { return iterator(ptr, bits, offset); }
  inline bool cas(const IDX x, const IDX exp) {
    return iterator_imp::cas<IDX, W>(x, exp, ptr, bits, offset);
  }
};

template<typename I, unsigned BITS, typename W, bool TS, unsigned UB>
void swap(setter<I, BITS, W, TS, UB> x, setter<I, BITS, W, TS, UB> y) {
  I t = x;
  x = (I)y;
  y = t;
}

} // namespace iterator_imp

template<typename IDX, typename W, bool TS, unsigned UB>
class iterator<IDX, 0, W, TS, UB> :
    public std::iterator<std::random_access_iterator_tag, IDX>,
    public iterator_imp::common<iterator<IDX, 0, W, TS, UB>, IDX, 0, W, UB>
{
  W*       m_ptr;
  unsigned m_bits;                // number of bits in an integral type
  unsigned m_offset;

  friend class iterator<IDX, 0, W, !TS, UB>;
  friend class const_iterator<IDX, 0, W, UB>;
  friend class iterator_imp::common<iterator<IDX, 0, W, TS, UB>, IDX, 0, W, UB>;
  friend class iterator_imp::common<const_iterator<IDX, 0, W, UB>, IDX, 0, W, UB>;

  typedef std::iterator<std::random_access_iterator_tag, IDX> super;
public:
  typedef typename super::value_type                          value_type;
  typedef typename super::difference_type                     difference_type;
  typedef IDX                                                 idx_type;
  typedef W                                                   word_type;
  typedef iterator_imp::setter<IDX, 0, W, TS, UB>             setter_type;

  iterator() = default;
  iterator(W* p, unsigned b, unsigned o)
    : m_ptr(p), m_bits(b), m_offset(o) { }
  template<unsigned BITS, bool TTS>
  iterator(const iterator<IDX, BITS, W, TTS>& rhs)
    : m_ptr(rhs.m_ptr), m_bits(rhs.m_bits), m_offset(rhs.m_offset) { }
  iterator(std::nullptr_t)
    : m_ptr(nullptr), m_bits(0), m_offset(0) { }

  setter_type operator*() { return setter_type(m_ptr, m_bits, m_offset); }
  setter_type operator[](const difference_type n) const {
    return *(*this + n);
  }

  // CAS val. Does NOT return existing value at pointer. Return true
  // if successful.
  inline bool cas(const IDX x, const IDX exp) {
    return iterator_imp::cas<IDX, W, UB>(x, exp, m_ptr, m_bits, m_offset);
  }

  unsigned bits() const { return m_bits; }
  void bits(unsigned b) { m_bits = b; }
};

template<typename IDX, typename W, unsigned UB>
class const_iterator<IDX, 0, W, UB> :
  public std::iterator<std::random_access_iterator_tag, const IDX>,
  public iterator_imp::common<const_iterator<IDX, 0, W, UB>, IDX, 0, W, UB>
{
  const W* m_ptr;
  unsigned m_bits;                // number of bits in an integral type
  unsigned m_offset;

  friend class iterator<IDX, 0, W>;
  friend class iterator_imp::common<iterator<IDX, 0, W, true, UB>, IDX, 0, W, UB>;
  friend class iterator_imp::common<iterator<IDX, 0, W, false, UB>, IDX, 0, W, UB>;
  friend class iterator_imp::common<const_iterator<IDX, 0, W, UB>, IDX, 0, W, UB>;

  typedef std::iterator<std::random_access_iterator_tag, IDX> super;
public:
  typedef typename super::value_type      value_type;
  typedef typename super::difference_type difference_type;
  typedef IDX idx_type;
  typedef W   word_type;


  const_iterator() = default;
  const_iterator(const W* p, unsigned b, unsigned o)
    : m_ptr(p), m_bits(b), m_offset(o) { }
  const_iterator(const const_iterator& rhs)
    : m_ptr(rhs.m_ptr), m_bits(rhs.m_bits), m_offset(rhs.m_offset) { }
  template<unsigned BITS, bool TS>
  const_iterator(const iterator<IDX, BITS, W, TS>& rhs)
    : m_ptr(rhs.m_ptr), m_bits(rhs.m_bits), m_offset(rhs.m_offset) { }
  const_iterator(std::nullptr_t)
    : m_ptr(nullptr), m_bits(0), m_offset(0) { }

  unsigned bits() const { return m_bits; }
  void bits(unsigned b) { m_bits = b; }
};

template<typename I, unsigned BITS, typename W, bool TS, unsigned UB>
struct const_iterator_traits<iterator<I, BITS, W, TS, UB>> {
  typedef const_iterator<I, BITS, W, UB> type;
};
template<typename I, unsigned BITS, typename W, unsigned UB>
struct const_iterator_traits<const_iterator<I, BITS, W, UB>> {
  typedef const_iterator<I, BITS, W, UB> type;
};

template<typename I, unsigned BITS, typename W, bool TS, unsigned UB>
struct parallel_iterator_traits<iterator<I, BITS, W, TS, UB>> {
  typedef iterator<I, BITS, W, true, UB> type;

  // Does a cas on iterator x. Weak though: val is NOT updated to the
  // current value.
  static inline bool cas(type& x, I& expected, const I& val) {
    return x.cas(val, expected);
  }
};

template<typename I, unsigned BITS, typename W, unsigned UB>
struct parallel_iterator_traits<const_iterator<I, BITS, W, UB>> {
  typedef const_iterator<I, BITS, W> type;
};

template<typename I, unsigned BITS, typename W, bool TS, unsigned UB>
struct prefetch_iterator_traits<iterator<I, BITS, W, TS, UB> > {
  template<int level = 0>
  static void read(const iterator<I, BITS, W, TS, UB>& p) { prefetch_iterator_traits<W*>::template read<level>(p.get_ptr()); }
  template<int level = 0>
  static void write(const iterator<I, BITS, W, TS, UB>& p) { prefetch_iterator_traits<W*>::template write<level>(p.get_ptr()); }

};

template<typename I, unsigned BITS, typename W, unsigned UB>
struct prefetch_iterator_traits<const_iterator<I, BITS, W, UB> > {
  template<int level = 0>
  static void read(const const_iterator<I, BITS, W, UB>& p) { prefetch_iterator_traits<const W*>::template read<level>(p.get_ptr()); }
  template<int level = 0>
  static void write(const const_iterator<I, BITS, W, UB>& p) { prefetch_iterator_traits<const W*>::template write<level>(p.get_ptr()); }

};

} // namespace compact

#endif /* __COMPACT_ITERATOR_H__ */

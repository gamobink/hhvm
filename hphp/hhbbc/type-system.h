/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/
#ifndef incl_HHBBC_TYPE_SYSTEM_H_
#define incl_HHBBC_TYPE_SYSTEM_H_

#include <cstdint>
#include <vector>
#include <utility>

#include <boost/container/flat_map.hpp>

#include <folly/Optional.h>

#include "hphp/util/copy-ptr.h"

#include "hphp/runtime/base/repo-auth-type.h"
#include "hphp/runtime/base/repo-auth-type-array.h"

#include "hphp/hhbbc/misc.h"
#include "hphp/hhbbc/index.h"

namespace HPHP { namespace HHBBC {

struct Type;

//////////////////////////////////////////////////////////////////////

/*
 * Type system.
 *
 * Here's an unmaintainable ascii-art diagram:
 *
 *                      Top
 *                       |
 *                 +-----+              InitGen :=  Gen - Uninit
 *                 |     |             InitCell := Cell - Uninit
 *                Cls   Gen---+              ?X := X + InitNull
 *                 |     |    |
 *              Cls<=c  Cell  Ref
 *                 |     |
 *              Cls=c    +-------------+--------+-------+-------+
 *                       |             |        |       |       |
 *                      Unc            |        |      Obj     Res
 *                       | \           |        |      /  \
 *                       |  \          |        |  Obj<=c Obj<=WaitHandle
 *                     Prim  \         |        |    |       |
 *                     / |   InitUnc   |        |  Obj=c   WaitH<T>
 *                    /  |   /  |  |   |        |
 *                   /   |  /   |  |   |        |
 *                  /    | /    |  |   |        |
 *                 /     |/     |  |   |        |
 *              Null  InitPrim  |  |   |        |
 *             /  |    / |      |  |  Arr      Str
 *            /   |   /  |      |  |  / \      / \
 *      Uninit  InitNull |      | SArr  ...   /  CStr
 *                       |      |  |         /
 *                       |      | ...       /
 *                       |      |          /
 *                       |      \         /
 *                       |       \       /
 *                       |        \     /
 *                       |         \   /
 *                       |          SStr
 *                       |           |
 *                       |         SStr=s
 *                       |
 *                       +----------+
 *                       |          |
 *                      Bool       Num
 *                      /  \       |  \
 *                   True  False  Int  Dbl
 *                                 |    |
 *                               Int=n Dbl=n
 *
 * Some notes on some of the basic types:
 *
 *   {Init,}Prim
 *
 *       "Primitive" types---these can be represented in a TypedValue without a
 *       pointer to the heap.
 *
 *   {Init,}Unc
 *
 *       "Uncounted" types---values of these types don't require reference
 *       counting.
 *
 *   WaitH<T>
 *
 *       A WaitHandle that is known will either return a value of type T from
 *       its join() method (or Await), or else throw an exception.
 *
 * Array types:
 *
 *   Arrays are divided along two dimensions: counted or uncounted, and empty
 *   or non-empty.  Unions of either are allowed.  The naming convention is
 *   {S,C,}Arr{N,E,}, where leaving out either bit means it's unknown along
 *   that dimension.  All arrays are subtypes of the Arr type.  The lattice
 *   here looks like this:
 *
 *                         Arr
 *                          |
 *                  +----+--+--+---+
 *                  |    |     |   |
 *                  |  SArr   CArr |
 *                  |   |      |   |
 *              +-------+---------------+
 *              |   |          |   |    |
 *              |  ArrN  +-----+  ArrE  |
 *              | /   \  |     | /   \  |
 *             SArrN  CArrN   CArrE  SArrE
 *
 *   "Specialized" array types may be found as subtypes of any of the above
 *   types except SArrE and CArrE, or of optional versions of the above types
 *   (e.g. as a subtype of ?ArrN).  The information about additional structure
 *   is dispayed in parenthesis, and probably best explained by some examples:
 *
 *     SArrN(Bool,Int)
 *
 *         Tuple-like static two-element array, with integer keys 0 and 1,
 *         containing a Bool and an Int with unknown values.
 *
 *     Arr(Int,Dbl)
 *
 *         An array of unknown countedness that is either empty, or a
 *         tuple-like array with two elements of types Int and Dbl.
 *
 *     CArrN([Bool])
 *
 *         Non-empty reference counted array with contiguous zero-based integer
 *         keys, unknown size, values all are subtypes of Bool.
 *
 *     ArrN(x:Int,y:Int)
 *
 *         Struct-like array with known fields "x" and "y" that have Int
 *         values, and no other fields.  Struct-like arrays always have known
 *         string keys, and the type contains only those array values with
 *         exactly the given key set.  The order of the array elements is not
 *         tracked in this type system.
 *
 *     Arr([SStr:InitCell])
 *
 *         Possibly empty map-like array with unknown keys, but all
 *         non-reference counted strings, all values InitCell.  In this case
 *         the array itself may or may not be static.
 *
 *         Note that struct-like arrays will be subtypes of map-like arrays
 *         with string keys.
 *
 *     ArrN([Int:InitPrim])
 *
 *         Map-like array with only integer keys (not-necessarily contiguous)
 *         and values that are all subtypes of InitPrim.  Note that the keys
 *         *may* be contiguous integers, so for example Arr([InitPrim]) <:
 *         Arr([Int => InitPrim]).
 *
 *     Arr([InitCell:InitCell])
 *
 *         Map-like array with either integer or string keys, and InitCell
 *         values, or empty.  Essentially this is the most generic array that
 *         can't contain php references.
 *
 *  TODO(#3774082): we should have a Str|Int type.
 */

//////////////////////////////////////////////////////////////////////

enum trep : uint32_t {
  BBottom   = 0,

  BUninit   = 1 << 0,
  BInitNull = 1 << 1,
  BFalse    = 1 << 2,
  BTrue     = 1 << 3,
  BInt      = 1 << 4,
  BDbl      = 1 << 5,
  BSStr     = 1 << 6,  // static string
  BCStr     = 1 << 7,  // counted string
  BSArrE    = 1 << 8,  // static empty array
  BCArrE    = 1 << 9,  // counted empty array
  BSArrN    = 1 << 10, // static non-empty array
  BCArrN    = 1 << 11, // counted non-empty array
  BObj      = 1 << 12,
  BRes      = 1 << 13,
  BCls      = 1 << 14,
  BRef      = 1 << 15,
  BSVecE    = 1 << 16, // static empty vec
  BCVecE    = 1 << 17, // counted empty vec
  BSVecN    = 1 << 18, // static non-empty vec
  BCVecN    = 1 << 19, // counted non-empty vec
  BSDictE   = 1 << 20, // static empty dict
  BCDictE   = 1 << 21, // counted empty dict
  BSDictN   = 1 << 22, // static non-empty dict
  BCDictN   = 1 << 23, // counted non-empty dict
  BSKeysetE = 1 << 24, // static empty keyset
  BCKeysetE = 1 << 25, // counted empty keyset
  BSKeysetN = 1 << 26, // static non-empty keyset
  BCKeysetN = 1 << 27, // counted non-empty keyset

  BNull     = BUninit | BInitNull,
  BBool     = BFalse | BTrue,
  BNum      = BInt | BDbl,
  BStr      = BSStr | BCStr,
  BSArr     = BSArrE | BSArrN,
  BCArr     = BCArrE | BCArrN,
  BArrE     = BSArrE | BCArrE,
  BArrN     = BSArrN | BCArrN,   // may have value / data
  BArr      = BArrE | BArrN,
  BSVec     = BSVecE | BSVecN,
  BCVec     = BCVecE | BCVecN,
  BVecE     = BSVecE | BCVecE,
  BVecN     = BSVecN | BCVecN,
  BVec      = BVecE | BVecN,
  BSDict    = BSDictE | BSDictN,
  BCDict    = BCDictE | BCDictN,
  BDictE    = BSDictE | BCDictE,
  BDictN    = BSDictN | BCDictN,
  BDict     = BDictE | BDictN,
  BSKeyset  = BSKeysetE | BSKeysetN,
  BCKeyset  = BCKeysetE | BCKeysetN,
  BKeysetE  = BSKeysetE | BCKeysetE,
  BKeysetN  = BSKeysetN | BCKeysetN,
  BKeyset   = BKeysetE | BKeysetN,


  // Nullable types.
  BOptTrue     = BInitNull | BTrue,
  BOptFalse    = BInitNull | BFalse,
  BOptBool     = BInitNull | BBool,
  BOptInt      = BInitNull | BInt,       // may have value
  BOptDbl      = BInitNull | BDbl,       // may have value
  BOptNum      = BInitNull | BNum,
  BOptSStr     = BInitNull | BSStr,      // may have value
  BOptCStr     = BInitNull | BCStr,
  BOptStr      = BInitNull | BStr,
  BOptSArrE    = BInitNull | BSArrE,
  BOptCArrE    = BInitNull | BCArrE,
  BOptSArrN    = BInitNull | BSArrN,     // may have value / data
  BOptCArrN    = BInitNull | BCArrN,     // may have value / data
  BOptSArr     = BInitNull | BSArr,      // may have value / data
  BOptCArr     = BInitNull | BCArr,      // may have value / data
  BOptArrE     = BInitNull | BArrE,      // may have value / data
  BOptArrN     = BInitNull | BArrN,      // may have value / data
  BOptArr      = BInitNull | BArr,       // may have value / data
  BOptObj      = BInitNull | BObj,       // may have data
  BOptRes      = BInitNull | BRes,
  BOptSVecE    = BInitNull | BSVecE,
  BOptCVecE    = BInitNull | BCVecE,
  BOptSVecN    = BInitNull | BSVecN,
  BOptCVecN    = BInitNull | BCVecN,
  BOptSVec     = BInitNull | BSVec,
  BOptCVec     = BInitNull | BCVec,
  BOptVecE     = BInitNull | BVecE,
  BOptVecN     = BInitNull | BVecN,
  BOptVec      = BInitNull | BVec,
  BOptSDictE   = BInitNull | BSDictE,
  BOptCDictE   = BInitNull | BCDictE,
  BOptSDictN   = BInitNull | BSDictN,
  BOptCDictN   = BInitNull | BCDictN,
  BOptSDict    = BInitNull | BSDict,
  BOptCDict    = BInitNull | BCDict,
  BOptDictE    = BInitNull | BDictE,
  BOptDictN    = BInitNull | BDictN,
  BOptDict     = BInitNull | BDict,
  BOptSKeysetE = BInitNull | BSKeysetE,
  BOptCKeysetE = BInitNull | BCKeysetE,
  BOptSKeysetN = BInitNull | BSKeysetN,
  BOptCKeysetN = BInitNull | BCKeysetN,
  BOptSKeyset  = BInitNull | BSKeyset,
  BOptCKeyset  = BInitNull | BCKeyset,
  BOptKeysetE  = BInitNull | BKeysetE,
  BOptKeysetN  = BInitNull | BKeysetN,
  BOptKeyset   = BInitNull | BKeyset,

  BInitPrim = BInitNull | BBool | BNum,
  BPrim     = BInitPrim | BUninit,
  BInitUnc  = BInitPrim | BSStr | BSArr | BSVec | BSDict | BSKeyset,
  BUnc      = BInitUnc | BUninit,
  BInitCell = BInitNull | BBool | BInt | BDbl | BStr | BArr | BObj | BRes |
              BVec | BDict | BKeyset,
  BCell     = BUninit | BInitCell,
  BInitGen  = BInitCell | BRef,
  BGen      = BUninit | BInitGen,

  BTop      = static_cast<uint32_t>(-1),
};

#define DATATAGS                                        \
  DT(Str, SString, sval)                                \
  DT(Int, int64_t, ival)                                \
  DT(Dbl, double, dval)                                 \
  DT(ArrVal, SArray, aval)                              \
  DT(Obj, DObj, dobj)                                   \
  DT(Cls, DCls, dcls)                                   \
  DT(RefInner, copy_ptr<Type>, inner)                   \
  DT(ArrPacked, copy_ptr<DArrPacked>, apacked)          \
  DT(ArrPackedN, copy_ptr<DArrPackedN>, apackedn)       \
  DT(ArrStruct, copy_ptr<DArrStruct>, astruct)          \
  DT(ArrMapN, copy_ptr<DArrMapN>, amapn)                \
  DT(Vec, copy_ptr<DVec>, vec)                          \
  DT(Dict, copy_ptr<DDict>, dict)                       \
  DT(Keyset, copy_ptr<DKeyset>, keyset)                 \
  DT(VecVal, SArray, vecval)                            \
  DT(DictVal, SArray, dictval)                          \
  DT(KeysetVal, SArray, keysetval)

// Tag for what kind of specialized data a Type object has.
enum class DataTag : uint8_t {
  None,
#define DT(name,...) name,
  DATATAGS
#undef DT
};

//////////////////////////////////////////////////////////////////////

/*
 * Information about a class type.  The class is either exact or a
 * subtype of the supplied class.
 */
struct DCls {
  enum Tag { Exact, Sub };

  DCls(Tag type, res::Class cls)
    : type(type)
    , cls(cls)
  {}

  Tag type;
  res::Class cls;
};

/*
 * Information about a specific object type.  The class is either
 * exact or a subtype of the supplied class.
 *
 * If the class is WaitHandle, we can also carry a type that joining
 * the wait handle will produce.
 */
struct DObj {
  enum Tag { Exact, Sub };

  DObj(Tag type, res::Class cls)
    : type(type)
    , cls(cls)
  {}

  Tag type;
  res::Class cls;
  copy_ptr<Type> whType;
};

struct DArrPacked;
struct DArrPackedN;
struct DArrStruct;
struct DArrMapN;
using StructMap = boost::container::flat_map<LSString,Type>;
struct DVec;
struct DDict;
struct DKeyset;

//////////////////////////////////////////////////////////////////////

struct Type {
  Type() : m_bits(BTop) {
    assert(checkInvariants());
  }
  explicit Type(trep t) : m_bits(t) {
    assert(checkInvariants());
  }

  Type(const Type&) noexcept;
  Type(Type&&) noexcept;
  Type& operator=(const Type&) noexcept;
  Type& operator=(Type&&) noexcept;
  ~Type() noexcept;

  /*
   * Exact equality or inequality of types, and hashing.
   */
  bool operator==(const Type& o) const;
  bool operator!=(const Type& o) const { return !(*this == o); }
  size_t hash() const;

  /*
   * Returns true if this type is definitely going to be a subtype or a strict
   * subtype of `o' at runtime.  If this function returns false, this may
   * still be a subtype of `o' at runtime, it just may not be known.
   */
  bool subtypeOf(const Type& o) const;
  bool strictSubtypeOf(const Type& o) const;

  /*
   * Subtype of any of the list of types.
   */
  template<class... Types>
  bool subtypeOfAny(const Type& t, Types... ts) const {
    return subtypeOf(t) || subtypeOfAny(ts...);
  }
  bool subtypeOfAny() const { return false; }

  /*
   * Returns whether there are any values of this type that are also
   * values of the type `o'.
   * When this function returns false, it is known that this type
   * must not be in any subtype relationship with the argument Type 'o'.
   * When true is returned the two types may still be unrelated but it is
   * not possible to tell.
   * Essentially this function can conservatively return true but must be
   * precise when returning false.
   */
  bool couldBe(const Type& o) const;

private:
  friend Type wait_handle(const Index&, Type);
  friend bool is_specialized_wait_handle(const Type&);
  friend bool is_specialized_array(const Type&);
  friend bool is_specialized_obj(const Type&);
  friend bool is_specialized_cls(const Type&);
  friend bool is_ref_with_inner(const Type&);
  friend Type wait_handle_inner(const Type&);
  friend Type sval(SString);
  friend Type ival(int64_t);
  friend Type dval(double);
  friend Type aval(SArray);
  friend Type subObj(res::Class);
  friend Type objExact(res::Class);
  friend Type subCls(res::Class);
  friend Type clsExact(res::Class);
  friend Type ref_to(Type);
  friend Type arr_packed(std::vector<Type>);
  friend Type sarr_packed(std::vector<Type>);
  friend Type carr_packed(std::vector<Type>);
  friend Type arr_packedn(Type);
  friend Type sarr_packedn(Type);
  friend Type carr_packedn(Type);
  friend Type arr_struct(StructMap);
  friend Type sarr_struct(StructMap);
  friend Type carr_struct(StructMap);
  friend Type arr_mapn(Type k, Type v);
  friend Type sarr_mapn(Type k, Type v);
  friend Type carr_mapn(Type k, Type v);
  friend DObj dobj_of(const Type&);
  friend DCls dcls_of(Type);
  friend Type union_of(Type, Type);
  friend Type widening_union(const Type&, const Type&);
  friend Type promote_emptyish(Type, Type);
  friend Type opt(Type);
  friend Type unopt(Type);
  friend bool is_opt(const Type&);
  friend folly::Optional<Cell> tv(const Type&);
  friend std::string show(const Type&);
  friend struct ArrKey disect_array_key(const Type&);
  friend Type array_elem(const Type&, const Type&);
  friend Type arrayN_set(Type, const Type&, const Type&);
  friend Type array_set(Type, const Type&, const Type&);
  friend std::pair<Type,Type> arrayN_newelem_key(const Type&, const Type&);
  friend std::pair<Type,Type> array_newelem_key(const Type&, const Type&);
  friend std::pair<Type,Type> iter_types(const Type&);
  friend RepoAuthType make_repo_type_arr(ArrayTypeTable::Builder&,
    const Type&);

  friend struct VecKey disect_vec_key(const Type&);
  friend struct StrictKey disect_strict_key(const Type&);

  friend std::pair<Type, bool> vec_elem(const Type&, const Type&);
  friend std::pair<Type, bool> vec_set(Type, const Type&, const Type&);
  friend Type vec_newelem(Type, const Type&);
  friend std::pair<Type, bool> dict_elem(const Type&, const Type&);
  friend std::pair<Type, bool> dict_set(Type, const Type&, const Type&);
  friend Type dict_newelem(Type, const Type&);
  friend std::pair<Type, bool> keyset_elem(const Type&, const Type&);
  friend std::pair<Type, bool> keyset_set(Type, const Type&, const Type&);
  friend Type keyset_newelem(Type, const Type&);

  friend Type spec_vec_union(Type, Type);
  friend Type spec_dict_union(Type, Type);
  friend Type spec_keyset_union(Type, Type);
  friend bool is_specialized_vec(const Type&);
  friend bool is_specialized_dict(const Type&);
  friend bool is_specialized_keyset(const Type&);
  friend Type toDVecType(Type);
  friend Type toDDictType(Type);
  friend Type toDKeysetType(Type);
  friend Type vec_val(SArray);
  friend Type dict_val(SArray);
  friend Type keyset_val(SArray);
  template<trep t> friend Type dict_n_impl(Type, Type);
  template<trep t> friend Type vec_n_impl(Type, folly::Optional<int64_t>);
  template<trep t> friend Type keyset_n_impl(Type);

private:
  union Data {
    Data() {}
    ~Data() {}

#define DT(tag_name,type,name) type name;
  DATATAGS
#undef DT
  };

  template<class Ret, class T, class Function>
  struct DJHelperFn;
  struct ArrKey;

private:
  static Type wait_handle_outer(const Type&);
  static Type unionArr(const Type& a, const Type& b);

private:
  template<class Ret, class T, class Function>
  DJHelperFn<Ret,T,Function> djbind(const Function& f, const T& t) const;
  template<class Ret, class T, class Function>
  Ret dj2nd(const Type&, DJHelperFn<Ret,T,Function>) const;
  template<class Function>
  typename Function::result_type disjointDataFn(const Type&, Function) const;
  bool hasData() const;
  bool equivData(const Type&) const;
  bool subtypeData(const Type&) const;
  bool couldBeData(const Type&) const;
  bool checkInvariants() const;

private:
  trep m_bits;
  DataTag m_dataTag = DataTag::None;
  Data m_data;
};

//////////////////////////////////////////////////////////////////////

struct DArrPacked {
  explicit DArrPacked(std::vector<Type> elems)
    : elems(std::move(elems))
  {}

  std::vector<Type> elems;
};

struct DArrPackedN {
  explicit DArrPackedN(Type t) : type(std::move(t)) {}
  Type type;
};

struct DArrStruct {
  explicit DArrStruct(StructMap map) : map(std::move(map)) {}
  StructMap map;
};

struct DArrMapN {
  explicit DArrMapN(Type key, Type val)
    : key(std::move(key))
    , val(std::move(val))
  {}
  Type key;
  Type val;
};

struct DVec {
  explicit DVec(Type t, folly::Optional<int64_t> s)
    : val(std::move(t))
    , len(std::move(s))
  {}

  Type val;
  folly::Optional<int64_t> len;
};

struct DDict {
  explicit DDict(Type k, Type v)
    : key(std::move(k))
    , val(std::move(v))
  {}

  Type key;
  Type val;
};

struct DKeyset {
  explicit DKeyset(Type kv) : keyval(std::move(kv)) {}
  Type keyval;
};

//////////////////////////////////////////////////////////////////////

#define X(y) const Type T##y = Type(B##y);

X(Bottom)

X(Uninit)
X(InitNull)
X(False)
X(True)
X(Int)
X(Dbl)
X(SStr)
X(CStr)
X(SArrE)
X(CArrE)
X(SArrN)
X(CArrN)
X(Obj)
X(Res)
X(Cls)
X(Ref)
X(SVecE)
X(CVecE)
X(SVecN)
X(CVecN)
X(SDictE)
X(CDictE)
X(SDictN)
X(CDictN)
X(SKeysetE)
X(CKeysetE)
X(SKeysetN)
X(CKeysetN)

X(Null)
X(Bool)
X(Num)
X(Str)
X(SArr)
X(CArr)
X(ArrE)
X(ArrN)
X(Arr)
X(SVec)
X(CVec)
X(VecE)
X(VecN)
X(Vec)
X(SDict)
X(CDict)
X(DictE)
X(DictN)
X(Dict)
X(SKeyset)
X(CKeyset)
X(KeysetE)
X(KeysetN)
X(Keyset)

X(InitPrim)
X(Prim)
X(InitUnc)
X(Unc)

X(OptTrue)
X(OptFalse)
X(OptBool)
X(OptInt)
X(OptDbl)
X(OptNum)
X(OptSStr)
X(OptCStr)
X(OptStr)
X(OptSArrE)
X(OptCArrE)
X(OptSArrN)
X(OptCArrN)
X(OptSArr)
X(OptCArr)
X(OptArrE)
X(OptArrN)
X(OptArr)
X(OptObj)
X(OptRes)
X(OptSVecE)
X(OptCVecE)
X(OptSVecN)
X(OptCVecN)
X(OptSVec)
X(OptCVec)
X(OptVecE)
X(OptVecN)
X(OptVec)
X(OptSDictE)
X(OptCDictE)
X(OptSDictN)
X(OptCDictN)
X(OptSDict)
X(OptCDict)
X(OptDictE)
X(OptDictN)
X(OptDict)
X(OptSKeysetE)
X(OptCKeysetE)
X(OptSKeysetN)
X(OptCKeysetN)
X(OptSKeyset)
X(OptCKeyset)
X(OptKeysetE)
X(OptKeysetN)
X(OptKeyset)

X(InitCell)
X(Cell)
X(InitGen)
X(Gen)

X(Top)

#undef X

//////////////////////////////////////////////////////////////////////

/*
 * Return WaitH<T> for a type t.
 */
Type wait_handle(const Index&, Type t);

/*
 * Return T from a WaitH<T>.
 *
 * Pre: is_specialized_handle(t);
 */
Type wait_handle_inner(const Type& t);

/*
 * Create Types that represent constant values.
 */
Type sval(SString);
Type ival(int64_t);
Type dval(double);
Type aval(SArray);
Type vec_val(SArray);
Type dict_val(SArray);
Type keyset_val(SArray);

/*
 * Create static empty array or string types.
 */
Type sempty();
Type aempty();
Type vec_empty();
Type dict_empty();
Type keyset_empty();

/*
 * Create a reference counted empty array/vec/dict.
 */
Type counted_aempty();
Type counted_vec_empty();
Type counted_dict_empty();
Type counted_keyset_empty();

/*
 * Create an any-countedness empty array/vec/dict type.
 */
Type some_aempty();
Type some_vec_empty();
Type some_dict_empty();
Type some_keyset_empty();

/*
 * Create types for objects or classes with some known constraint on
 * which res::Class is associated with them.
 */
Type subObj(res::Class);
Type objExact(res::Class);
Type subCls(res::Class);
Type clsExact(res::Class);

/*
 * Packed array types with known size.
 *
 * Pre: !v.empty()
 */
Type arr_packed(std::vector<Type> v);
Type sarr_packed(std::vector<Type> v);
Type carr_packed(std::vector<Type> v);

/*
 * Packed array types of unknown size.
 *
 * Note that these types imply the arrays are non-empty.
 */
Type arr_packedn(Type);
Type sarr_packedn(Type);
Type carr_packedn(Type);

/*
 * Struct-like arrays.
 *
 * Pre: !m.empty()
 */
Type arr_struct(StructMap m);
Type sarr_struct(StructMap m);
Type carr_struct(StructMap m);

/*
 * Map-like arrays.
 */
Type arr_mapn(Type k, Type v);
Type sarr_mapn(Type k, Type v);
Type carr_mapn(Type k, Type v);

/*
 * Vec type of optionally known size.
 */
Type vec_n(Type, folly::Optional<int64_t>);
Type cvec_n(Type, folly::Optional<int64_t>);
Type svec_n(Type, folly::Optional<int64_t>);

/*
 * Dict with key/value types.
 */
Type dict_n(Type, Type);
Type cdict_n(Type, Type);
Type sdict_n(Type, Type);

/*
 * Keyset with key (same as value) type.
 */
Type keyset_n(Type);
Type ckeyset_n(Type);
Type skeyset_n(Type);

/*
 * Create the optional version of the Type t.
 *
 * Pre: there must be an optional version of the type t.
 */
Type opt(Type t);

/*
 * Return the non-optional version of the Type t.
 *
 * Pre: is_opt(t)
 */
Type unopt(Type t);

/*
 * Returns whether a given type is a subtype of one of the predefined
 * optional types.  (Note that this does not include types like
 * TInitUnc---it's only the TOpt* types.)
 */
bool is_opt(const Type& t);

/*
 * Returns true if type 't' represents a "specialized" object, that is an
 * object of a known class, or an optional object of a known class.
 */
bool is_specialized_obj(const Type&);

/*
 * Returns true if type 't' represents a "specialized" class---i.e. a class
 * with a DCls structure.
 */
bool is_specialized_cls(const Type&);

/*
 * Returns whether `t' is a WaitH<T> or ?WaitH<T> for some T.
 *
 * Note that this function returns false for Obj<=WaitHandle with no
 * tracked inner type.
 */
bool is_specialized_wait_handle(const Type& t);

/*
 * Returns whether `t' is a Arr=something or Arr(something) type, or
 * an optional version of one of those types.  That is, an array with
 * either a constant value or some (maybe partially) known shape.
 */
bool is_specialized_array(const Type& t);

/*
 * Returns the best known TCls subtype for an object type.
 *
 * Pre: t.subtypeOf(TObj)
 */
Type objcls(const Type& t);

/*
 * If the type t has a known constant value, return it as a Cell.
 * Otherwise return folly::none.
 *
 * The returned Cell can only contain non-reference-counted types.
 */
folly::Optional<Cell> tv(const Type& t);

/*
 * Get the type in our typesystem that corresponds to an hhbc
 * IsTypeOp.
 *
 * Pre: op != IsTypeOp::Scalar
 */
Type type_of_istype(IsTypeOp op);

/*
 * Return the DObj structure for a strict subtype of TObj or TOptObj.
 *
 * Pre: is_specialized_obj(t)
 */
DObj dobj_of(const Type& t);

/*
 * Return the DCls structure for a strict subtype of TCls.
 *
 * Pre: is_specialized_cls(t)
 */
DCls dcls_of(Type t);

/*
 * Create a Type from a Cell.
 *
 * Pre: the cell must contain a non-reference-counted type.
 * Post: returned type is a subtype of TUnc
 */
Type from_cell(Cell tv);

/*
 * Create a Type from a DataType. KindOfString and KindOfPersistentString
 * are both treated as TStr.
 *
 * Pre: dt is one of the DataTypes that actually represent php values
 * (or KindOfUninit).
 */
Type from_DataType(DataType dt);

/*
 * Create a Type from a builtin type specification string.
 *
 * This is used for HNI class properties.  We assume that these are
 * accurate.  `s' may be nullptr.
 */
Type from_hni_constraint(SString s);

/*
 * Make a type that represents values from either of the supplied
 * types.
 *
 * Importantly, note that there are infinitely long chains of array
 * types that continue to become less specialized, so chains of
 * union_of operations are not guaranteed to reach a stable point in
 * finite steps.
 */
Type union_of(Type a, Type b);

/*
 * Widening union.
 *
 * This operation returns a type T, such that a is a subtype of T, b
 * is a subtype of T, and union_of(a, b) is a subtype of T.  The
 * widening union also has the property that every possible chain of
 * successive applications of the function eventually reaches a stable
 * point.
 *
 * For portions of our analysis that rely on growing types reaching
 * stable points for termination, this function must occasionally be
 * used instead of union_of to guarantee termination.  See details in
 * analyze.cpp.
 */
Type widening_union(const Type& a, const Type& b);

/*
 * A sort of union operation that also attempts to remove "emptyish" types from
 * union_of(a, b).  This is useful for promoting emptyish types (sempty(),
 * false, and null) to stdClass or to arrays, in member instructions.
 *
 * This function currently doesn't give specific guarantees about exactly when
 * the emptyish types will not be part of the return type (informally it only
 * happens in the "easy" cases right now), so you should not use it in
 * situations where union_of(a, b) would not also be correct.
 */
Type promote_emptyish(Type a, Type b);

/*
 * Returns the smallest type that `a' is a subtype of, from the
 * following set: TGen, TInitCell, TRef, TUninit, TCls.
 *
 * Pre: `a' is a subtype of TGen, or TCls.
 */
Type stack_flav(Type a);

/*
 * Force any type that contains SStr and SArr to contain Arr and Str.
 * This is needed for some operations that can change static arrays or
 * strings into non-static ones.  Doesn't change the type if it can't
 * contain SStr or SArr.
 */
Type loosen_statics(Type);

/*
 * Force any type that corresponds to a constant php value to contain
 * all values of that php type.
 *
 * Precisely: strict subtypes of TInt, TDbl, TBool, TSStr, and TSArr
 * become exactly that corresponding type.  Additionally, TOptTrue and
 * TOptFalse become TOptBool.  All other types are unchanged.
 *
 * TODO(#3696042): loosen values of an array shape should keep the
 * shape.
 */
Type loosen_values(Type);

/*
 * If t contains TUninit, returns the best type we can that contains
 * at least everything t contains, but doesn't contain TUninit.  Note
 * that this function will return TBottom for TUninit.
 *
 * Pre: t.subtypeOf(TCell)
 */
Type remove_uninit(Type t);

/*
 * Returns the best known type of an array inner element given a type
 * for the key.  The returned type is always a subtype of TInitCell.
 *
 * Pre: arr.subtypeOf(TArr)
 */
Type array_elem(const Type& arr, const Type& key);

/*
 * Perform an array set on types.  Returns a type that represents the
 * effects of arr[key] = val.
 *
 * Pre arr.subtypeOf(TArr)
 */
Type array_set(Type arr, const Type& key, const Type& val);

/*
 * Perform a newelem operation on an array type.  Returns an array
 * that contains a new pushed-back element with the supplied value, in
 * the sense of arr[] = val.
 *
 * Pre: arr.subtypeOf(TArr)
 */
Type array_newelem(const Type& arr, const Type& val);

/*
 * The same as array_newelem, except return the best known type of the
 * key that was added.  (This is either TInt or a subtype of it.)
 */
std::pair<Type,Type> array_newelem_key(const Type& arr, const Type& val);

/*
 * Returns the best known type for a hack array inner element given a type for
 * the key. The type returned will be TBottom if the operation will always
 * throw, and the bool will be true if the operation will never throw.
 */
std::pair<Type, bool> vec_elem(const Type& vec, const Type& key);
std::pair<Type, bool> dict_elem(const Type& dict, const Type& key);
std::pair<Type, bool> keyset_elem(const Type& keyset, const Type& key);

/*
 * Perform a set operation on a hack array. Returns a type that represents the
 * effects of $a[key] = val, for a hack array $a.
 *
 * The returned type will be TBottom if the operation will always throw, and
 * the bool will be true if the operation can never throw.
 */
std::pair<Type, bool> vec_set(Type vec, const Type& key, const Type& val);
std::pair<Type, bool> dict_set(Type dict, const Type& key, const Type& val);
std::pair<Type, bool> keyset_set(Type keyset, const Type& key, const Type& val);

/*
 * Perform a newelem operation on a hack array type. Returns a new type which
 * represents the result of this operation.
 */
Type vec_newelem(Type vec, const Type& val);
Type dict_newelem(Type dict, const Type& val);
Type keyset_newelem(Type keyset, const Type& val);

/*
 * Return the best known key and value type for iteration of the
 * supplied type.  This is only intended for non-mutable iteration, so
 * the returned types are at worst InitCell.
 */
std::pair<Type,Type> iter_types(const Type&);

/*
 * Create a RepoAuthType for a Type.
 *
 * RepoAuthTypes may contain things like RepoAuthType::Array*'s or
 * SStrings for class names.  The emit code needs to handle making
 * sure these things are merged into the appropriate unit or repo.
 *
 * Pre: !t.couldBe(TCls)
 *      !t.subtypeOf(TBottom)
 */
RepoAuthType make_repo_type(ArrayTypeTable::Builder&, const Type& t);

//////////////////////////////////////////////////////////////////////

}}

#endif

/* Copyright (c) 2012, 2020, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#pragma once

// OLEGS: implement cross-platform
#define MY_MT_INCR(x)                                                           \
  _InterlockedIncrement(reinterpret_cast<volatile long *>(&x))
#define MY_MT_DECR(x)                                                           \
  _InterlockedDecrement(reinterpret_cast<volatile long *>(&x))

/* Base class for reference counting */
class Reference_counter_base
{
protected:
  Reference_counter_base() noexcept= default;

public:
  /* Forbid copying and moving */
  Reference_counter_base(const Reference_counter_base &)= delete;
  Reference_counter_base(Reference_counter_base &&)= delete;
  Reference_counter_base &operator=(const Reference_counter_base &)= delete;
  Reference_counter_base &operator=(Reference_counter_base &&)= delete;

  virtual ~Reference_counter_base() noexcept { }

  // Increment use count
  void incref() noexcept
  {
    MY_MT_INCR(uses);
  }

  // Decrement use count
  void decref() noexcept
  {
    if (MY_MT_DECR(uses) == 0)
    {
      destroy();
    }
  }

  unsigned long use_count() const noexcept { return uses; }

private:
  virtual void destroy() noexcept= 0; /* destroy managed resource */

  unsigned long uses= 1;
};


template <typename Ty>
class Reference_counter : public Reference_counter_base
{ 
public:
  explicit Reference_counter(Ty *obj) :
    Reference_counter_base(), managed_obj(obj) 
  {}

private:
  virtual void destroy() noexcept override
  { 
    /* destroy managed resource */
    delete managed_obj;
  }

  Ty *managed_obj;
};


template <typename Ty> class Shared_ptr;

// Base class for shared_ptr and weak_ptr
template <typename Ty> class Ptr_base
{
public:
  long use_count() const noexcept
  {
    return ref_counter ? ref_counter->use_count() : 0;
  }

  /* Forbid copying and moving */
  Ptr_base(const Ptr_base &)= delete;
  Ptr_base(Ptr_base &&)= delete;
  Ptr_base &operator=(const Ptr_base &)= delete;
  Ptr_base &operator=(Ptr_base &&)= delete;

protected:
  Ty *get() const noexcept { return managed_obj; }

  Ptr_base() noexcept= default;

  // shared_ptr's (converting) move ctor
  template <typename Ty2>
  void move_construct_from(Ptr_base<Ty2> &&other) noexcept
  {
    managed_obj= other.managed_obj;
    ref_counter= other.ref_counter;

    other.managed_obj= nullptr;
    other.ref_counter= nullptr;
  }

  // Shared_ptr's (converting) copy ctor
  template <typename Ty2>
  void copy_construct_from(const Shared_ptr<Ty2> &other) noexcept
  {
    other.incref();

    managed_obj= other.managed_obj;
    ref_counter= other.ref_counter;
  }

  void incref() const noexcept
  {
    if (ref_counter)
    {
      ref_counter->incref();
    }
  }

  void decref() noexcept
  { 
    if (ref_counter)
    {
      ref_counter->decref();
    }
  }

  void swap(Ptr_base &right) noexcept
  { 
    auto tmpObj= this->managed_obj;
    this->managed_obj= right.managed_obj;
    right.managed_obj= tmpObj;

    auto tmpRefCnt= this->ref_counter;
    this->ref_counter= right.ref_counter;
    right.ref_counter= tmpRefCnt;
  }
  
private:
  Ty *managed_obj= nullptr;
  Reference_counter_base *ref_counter= nullptr;

  template <class Ty0> friend class Ptr_base;

  friend Shared_ptr<Ty>;
};


// OLEGS: prevent creation for arrays like: Shared_ptr<int> p(new int(100))

// class for reference counted resource management
template <typename Ty> class Shared_ptr : public Ptr_base<Ty>
{ 
public:
  Shared_ptr() noexcept= default;

  // construct empty shared_ptr
  Shared_ptr(nullptr_t) noexcept {}

  // construct Shared_ptr object that owns object obj
  template <typename Ux> explicit Shared_ptr(Ux *obj)
  { 
    this->managed_obj= obj;
    this->ref_counter= new Reference_counter<Ux>(obj);
  }

  // construct shared_ptr object that owns same resource as other
  Shared_ptr(const Shared_ptr &other) noexcept
  {
    this->copy_construct_from(other);
  }

  // construct shared_ptr object that takes resource from right
  Shared_ptr(Shared_ptr &&right) noexcept
  {
    this->move_construct_from(std::move(right));
  }

  ~Shared_ptr() noexcept
  { 
    // release resource
    this->decref();
  }

  Shared_ptr &operator=(const Shared_ptr &right) noexcept
  {
    Shared_ptr(right).swap(*this);
    return *this;
  }

  template <class Ty2>
  Shared_ptr &operator=(const Shared_ptr<Ty2> &right) noexcept
  {
    Shared_ptr(right).swap(*this);
    return *this;
  }

  Shared_ptr &operator=(Shared_ptr &&right) noexcept
  {
    Shared_ptr(std::move(right)).swap(*this);
    return *this;
  }

  template <class Ty2>
  Shared_ptr &operator=(Shared_ptr<Ty2> &&right) noexcept
  {
    Shared_ptr(std::move(right)).swap(*this);
    return *this;
  }

  void swap(Shared_ptr &other) noexcept { Ptr_base<Ty>::swap(other); }

  // release resource and convert to empty shared_ptr object
  void reset() noexcept
  {
    Shared_ptr().swap(*this);
  }

  // release, take ownership of obj
  template <typename Ux> void reset(Ux *obj)
  {
    Shared_ptr(obj).swap(*this);
  }

  using Ptr_base<Ty>::get;

  template<typename Ty2= Ty>
  Ty2 &operator*() const noexcept
  {
    return *get();
  }

  template <typename Ty2= Ty>
  Ty2 *operator->() const noexcept
  {
    return get();
  }

  explicit operator bool() const noexcept
  {
    return get() != nullptr;
  }

private:
  /* template <class _UxptrOrNullptr, class _Dx>
  void _Setpd(const _UxptrOrNullptr _Px, _Dx _Dt)
  { // take ownership of _Px, deleter _Dt
    _Temporary_owner_del<_UxptrOrNullptr, _Dx> _Owner(_Px, _Dt);
    _Set_ptr_rep_and_enable_shared(
        _Owner._Ptr, new _Ref_count_resource<_UxptrOrNullptr, _Dx>(
                         _Owner._Ptr, _STD move(_Dt)));
    _Owner._Call_deleter= false;
  }

  template <class _UxptrOrNullptr, class _Dx, class _Alloc>
  void _Setpda(const _UxptrOrNullptr _Px, _Dx _Dt, _Alloc _Ax)
  { // take ownership of _Px, deleter _Dt, allocator _Ax
    using _Alref_alloc= _Rebind_alloc_t<
        _Alloc, _Ref_count_resource_alloc<_UxptrOrNullptr, _Dx, _Alloc>>;

    _Temporary_owner_del<_UxptrOrNullptr, _Dx> _Owner(_Px, _Dt);
    _Alref_alloc _Alref(_Ax);
    _Alloc_construct_ptr<_Alref_alloc> _Constructor(_Alref);
    _Constructor._Allocate();
    _Construct_in_place(*_Constructor._Ptr, _Owner._Ptr, _STD move(_Dt), _Ax);
    _Set_ptr_rep_and_enable_shared(_Owner._Ptr, _Unfancy(_Constructor._Ptr));
    _Constructor._Ptr= nullptr;
    _Owner._Call_deleter= false;
  }

  template <class _Ty0, class... _Types>
  friend shared_ptr<_Ty0> make_shared(_Types &&..._Args);
  
  template <typename Ux>
  void _Set_ptr_rep_and_enable_shared(Ux *const _Px,
                                      Reference_counter_base *const _Rx) noexcept
  { // take ownership of _Px
    this->_Ptr= _Px;
    this->_Rep= _Rx;
  }

  void _Set_ptr_rep_and_enable_shared(nullptr_t,
                                      _Ref_count_base *const _Rx) noexcept
  { // take ownership of nullptr
    this->_Ptr= nullptr;
    this->_Rep= _Rx;
  }*/
};

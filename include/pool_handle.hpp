#pragma once

#include<cassert>//assert



/* The pool we created works, but there is a possibility of memory leak
    ObjectPool<Particle,1024>pool;
    Particle* p=pool.acquire(x,y);
    //the issue is forgetting to release p (i.e pool.release(p)) when we are done with p (for eg we return from function in between after creating p (the function has parameter of pool by referdnce))
    // Every break, return ,throw is an opportunity to leak slot and pool leak is worse than heap leak. Leak enough and acquire() starts throwing pool_exhausted in production 
    
    what we want-
    auto h=pool.make(10.0f,20.0f);//acquire + wrap in 1 step
    h->x+=1.0f; (*h).y=5.0f;// h is like a pointer to your Particle object but more like a unique ptr
    As h goes out of scope it automatically calls delete preventing memory leaks

    Note: ownership is transferable never shared
    auto h1=pool.make(1.0f,2.0f);
    auto h2=std::move(h1);//ownership transferred, h1 is now empty and points nowhere in pool
    auto h3=h1;//compiler error - cant copy bcz two handles owning one slot would both call release() in their destructors (double release ) unless refcounting is maintained (But we dont want overhead of shared_ptr)
    //We dont want shared ownership bcz pooled objects are almost always short lived and single owner -particles, network messages, order objects etc. 
    //sharing the ownership would heap allocate a control block (needed for refcounting ) per acquire() - the exact allocation the pool exists to remove 
    //It will also make release time non deterministic (slot comes back whenever the last owner happens to drop it ) which kills LIFO cache locality that makes reuse faster. 
    //If we implement unique handler, a user who needs sharing can get sharing by adding one line -i.e using shared pointer to point to h (so no need of adding sharing 
    
    std::vector<PoolHandle<..>>v; v.push_back(pool.make(0.f,0.f));//moved into vector instead of copying coz copied is restricted (vector tries to use move whenever move is required)
    v.clear();//all slots returned to pool

    Particle* borrowed=h.get();//borrowed points to the object in the pool handler, 

*/
namespace objpool{
//Pool Handler- An owning, move-only handle to one object acquired from a Pool
//
//Semantically it is a std::unique_ptr<T> whose deleter is "return this slot to pool" (instead of delete the memory from heap which is the default delete is of unique ptr)
//Two pointers wide; no allocation, no reference couunting 
// LIFETIME: a pool Handle must never outlive its pool. Declare the pool first, so reverse destruction order tears the handle down before pool gets destroyed
//                  ObjectPool<Particle,64>pool;  //destroyed Last
//                  auto h=pool.make(1.0f,2.0f); //destoryed first
//Invariant : ptr_!=nullptr implies pool_!=nullptr (whenever the handle owns the object i.e ptr_!=nullptr, it must also know the pool that owns that object pool_!=nullptr)


template<typename T, typename Pool>
class PoolHandle{
public:
    //empty handle -owns nothing
    PoolHandle() noexcept=default;

    explicit PoolHandle(T* ptr,Pool* pool) noexcept : ptr_{ptr}, pool_{pool} {
        //any constructor that acquires a resource should be explicit, ownership transfer must be visible at the call site 
        // Loose<int>h={&slot,&pool} ->this reads like we are initializing some object h, but instead here we are making the object h by transfering ownership of pool(this is ownership acquisition)
        //we write explicit for this purpose only so that code remains readable and doesnt become ambiguous 
        assert((ptr==nullptr||pool!=nullptr) && "PoolHandle: a non-null pointer requires a non-null pool");
        assert((ptr==nullptr||pool->owns(ptr)) && "PoolHandle: pointer doesn't belong to this pool");
    }

    ~PoolHandle(){
        if(ptr_!=nullptr){//if ptr_ !=nullptr=> pool_!=null so dont need to check pool_!=null specifically and if ptr_ is already null, nothing needs releasing regardless of pool
            pool_->release(ptr_);
        }
    }

    //------move constructor-----------------------
    //take the source's ownership and leave the source empty (nulling 'other' is not cleanum, it is what makes this a move instead of copy)
    //if you dont leave the source empty both handles will later release the same slot
    PoolHandle(PoolHandle&& other) noexcept : ptr_{other.ptr_}, pool_{other.pool_} {
        other.ptr_=nullptr;
        other.pool_=nullptr;//when ptr_=nullptr , the handle owns nothing , so there is nothing to return . Therefore knowing the pool serves no purpose in this design 
    }

    //-----move assignment-------------
    //Unlike the constructor, '*this' may already own something (since we already have a constructed object on LHS of =)
    //1. self assignment check 2. release what we hold 3. steal +null the source
    PoolHandle& operator=(PoolHandle&& other) noexcept{
        if(this!=&other){  //h=std::move(h) must not destroy our object coz second step is to release what we hold
            if(this->ptr_!=nullptr)this->pool_->release(ptr_); //drop our current object or it will leak since no one will release(ptr_)
            this->ptr_=other.ptr_;
            this->pool_=other.pool_;
            other.ptr_=nullptr;
            other.pool_=nullptr;
        }
        return *this;  
    }
    PoolHandle(const PoolHandle&)=delete;//delete copy constructor
    PoolHandle& operator=(const PoolHandle&)=delete; //delete copy assignment operator 
private:
    T* ptr_{nullptr};//pointer to the object of type T which is stored inside the object Pool
    Pool* pool_{nullptr}; //pointer to the pool which owns that particular object of type T (which the ptr_ points to )
};

}
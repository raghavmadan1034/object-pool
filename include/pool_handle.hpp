#pragma once

#include<cassert>//assert
#include<utility>//swap
#include<cstddef>//


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

//handler should feel just like a unique pointer to the object and is responsible for cleaning the object : The only difference is that handler allocates the object on the object pool
//whereas the unique pointer handles  the object allocated on heap . Handler is analogically making a new call only to the object but is using the object pool instead
//Pool* pool_ is only the reference to a pre-existing object pool [pointer gives the address, doesnt mean that pool is allocated on heap] (handler never creates or destroys the pool)
//handle owns a lifetime, it points into storage (which the pool owns) where the objects sit , and cleans up by runnng ~T() and returning the slot (No delete or new calls for object pool since our source of memeory is pool not heap)
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

    //-----------observers(access to the owned objects)----------------
    //Note- these are const memeber functions returning non const T& and T* ; const handle means "you shall not change which object I own inside fxn " (you cant change class's memeber variable inside const function, also non memeber functions(fxn outside a class) can never be const) ,but not that "the object is constant"
    //T* const : the pointer is a constant , the pointee can change (std::uniqu_ptr behaves exactly)
    
    [[nodiscard]] T& operator*() const noexcept{ //dereference operator 
        assert(ptr_!=nullptr && "PoolHandle: derefenced an empty handle");
        return *ptr_;
    }
    [[nodiscard]] T* operator->() const noexcept{
        //h->x compiles to h.operator->()->x => ptr_->x : the compiler keeps applying -> until it reaches a raw pointer
        assert(ptr_!=nullptr && "PoolHandle: member access on an empty handle");
        return ptr_;
    }

    //Non-owning borrow.The caller must not call pool.release() on this pointer retuned - the handler still owns it 
    //handler temporarily hands you the memeory address it is managing (address where the  object of type T is present) so you can look at it and use it, but the handler retains full control over releasing it 
    [[nodiscard]]  T* get() const noexcept{
        return ptr_;
    }
    [[nodiscard]] Pool* pool() const noexcept{//returns the pointer to pool 
        return pool_; //note - we never return the pointer by reference , user can change the pointed object but if user changes the returned pointer, it will have no effect on the current objects pointer 
    }

    //overload operator bool - to enable if(h) and if(!h)
    explicit operator bool() const noexcept{
        return ptr_!=nullptr;
    }
    //Note- No sense to define operator== , why ? Null test already covered using operator bool and PoolHandle is move only and uniquely owning ,so h1==h2 will always be false (2 distinct live handles can never point at same object)
    //if someone writes h1==h2 , it will raise compile error bcz our operator bool is explicit and there is no operator== defined.
    //we can still easily do h.get()==raw since this involves raw pointers 

    //--------ownership surrender---------------------------------
    //Stop owning Without releasing the object from the object pool.
    //The slot is now the caller's debt, they must eventually call pool.release() on it or the slot will be lost until pool dies
    //[[nodiscard]] bcz discarding the return value leaks the slot with no way to recover it 
    //usage order - Pool* p=h.pool(); T* raw=h.detach(); ........;p->release(raw);
    [[nodiscard]] T* detach() noexcept{
        T* out=ptr_;
        ptr_=nullptr;
        pool_=nullptr;//surrender the object (and pool doesnt make any sense if there is no object i.e surrender the entire handler)
        return out;
    }

    //reset()- release back to the pool whatever we currently own and then optionally adopt a new object
    void reset(T* ptr=nullptr, Pool* pool=nullptr) noexcept{
        assert((ptr==nullptr||pool!=nullptr) && "PoolHandle: a non-null pointer requires a non-null pool");
        //cache the old,m install the new, release the old last
        //pool.release() runs ~T(), which is an arbitarary user code
        //why dont we do pool_->release(ptr_) and then assign ptr_=ptr and pool_=pool ?
        //pool_->release() calls ~T() and inside the destructor if code is written like : if(h){//do something utilizing the handle, maybe free handle}
        //since we havent set h.ptr_=nullptr yet, if(h) evaluates to true and actually executes the code written inside the branch . And if inside branch again h.reset() is called it will double release
        //however this is rare and but can happen 
        T* old=ptr_;
        Pool* old_pool=pool_;
        ptr_=ptr;
        pool_=pool;
        if(old!=nullptr)old_pool->release(old);
        //now when inside ~T() it compares if(h.get()==this) it evaluates to false and we are safe
    }  
    // is reset self safe? i.e h.reset(h.get(),h.pool()) - it will leave a dangling handler, it will release the object from the memory pool, but pool_ will point to the address of memoery pool, and ptr_ will point to the location in memeory pool in which object earlier was
    //and now there sits a Node object .(unique_ptr has the identical flaw ) - so dereferencing ptr_ is undefined behaviour bcz ptr_ doesnt point to a T* object

    //swap 
    void swap(PoolHandle& other) noexcept{
        std::swap(ptr_,other.ptr_);
        std::swap(pool_,other.pool_);
    }
private:
    T* ptr_{nullptr};//pointer to the object of type T which is stored inside the object Pool
    Pool* pool_{nullptr}; //pointer to the pool which owns that particular object of type T (which the ptr_ points to )
};

//Non-member swap ; why cant we do std::swap(h1,h2) ?
//generic std::swap would do , tmp=std::move(h1), h1=std::move(h2) , h2=std::move(h1) -1 move constructor + 2 move assignment operators - and move assignment involves release() operation 
template<typename T, typename Pool>
void swap(PoolHandle<T,Pool>&a, PoolHandle<T,Pool>&b) noexcept{
    a.swap(b);
}
//ADL- argument dependent lookup. When you call a function w/o specicifying its namespace like just : swap(a,b)
//the compiler searches the namespace where the arguments were defined
//bcz a is objpool::PoolHandle and b is objpool::Poolhandle , compiler automatically searches in objpool namespace and finds objpool::swap() function
//if you explicitl;y use std::swap(a,b) compiler uses the generic swap inside STL
//if you just write swap(a,b), it works correctly if parameters are objpool::PoolhHandle, but gives compiler error if a and b are int
//so best way to use swap inside a function when you dont know parameters is:
// void function(auto& a,auto &b){
//     using std::swap;//provides a fallback in case a and b are not PoolHandle types (using std::swap; imports only swap function from STL)
//     swap(a,b);//ADL may find a better custom swap
// }


//convinience comparisons against nullptr, so h==nullptr reads naturally alopngside if(!h)
//These only accept nullptr (std::nullptr_t has exactly one value {std::nullptr_t is dedicated type used to represent universal null-pointer literal[nullptr is not a int* or void*, it can convert to any pointer type]} )
//so h1==h2 remains compile error (you cant say h=null really bcz h is not exactly a pointer, we are additng this h==nullptr for comfort of user)
template<typename T, typename Pool>
bool operator==(const PoolHandle<T,Pool>&h, std::nullptr_t) noexcept{
    return h.get()==nullptr;
}
template<typename T, typename Pool>
bool operator==(std::nullptr_t, const PoolHandle<T,Pool>&h) noexcept{
    return h.get()==nullptr;
}
template<typename T, typename Pool>
bool operator!=(const PoolHandle<T,Pool>&h, std::nullptr_t) noexcept{
    return h.get()!=nullptr;
}
template<typename T, typename Pool>
bool operator!=(std::nullptr_t, const PoolHandle<T,Pool>&h) noexcept{
    return h.get()!=nullptr;
}
//C++20 has a spaceship operator <=> which can overload <,>,<=,>= operators in one go. C++ operator== handles != as well for both sides - but since we are using C++17, we need to define these 4 seperately 
}
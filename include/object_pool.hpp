#pragma once

#include<cstddef> //std::byte, size_t
#include<utility>  //std::forward
#include<type_traits>  //std:: is_trivially_destructible_v
#include<new>   //placement new, std::bad_alloc
#include<memory>  //std::destroy_at
#include<algorithm> //std::max
#include<cstdlib> //std::abort
#include<cassert> //assert
#include<cstdint> //std::uintptr_t,SIZE_MAX
/*  ---What we want

ObjectPool<Particle,1024>pool;  //1024 objects of type Particle, memory reserved now   - capacity is a template parameter (not an Argument) bcz storage size must be known at compile time to live inside the object(one allocationa nd then never again)
{
    auto h=pool.aquire(10.0,20.0);// construct Particle(10,20)  - similar to emplace back, perfect-forwards the constructor arguments and constructs directly in the slot.
    //for this we shall use variadic templates, it will still accept a pre constructed object if the caller has one eg - Particle p(10,20); pool.aquire(std::move(p))

    h->x+=1.0; //use it like a pointer
    //aquire() returns a handler not T* (a raw pointer cant clean up itself, we use smart pointer handler)
}//auto returned to the pool here

auto h2=pool.aquire(0.0,0.0);//reuses the same slot
*/
namespace objpool{ //good habit to define namespace, thousands of codebases use ObjectPool  eg- #include<game_engine/memory.h> also defines an ObjectPool (for multiple libraries to coexist namespace is necessary)

//RAII handle is returned by aquire, declearing it here (defined in pool_handle.hpp)
//handler needs only T and pool's type -not capacity or policy
template<typename T, typename Pool>
class PoolHandle;
//Handler behaves like a unique pointer 
//if two handlers point to 2 memory pools of same type (i.e same parameters, class and Capacity) then we can use move operation on one of the handles so that other handler points to that (But copy assignment operation is compile time error)
//however using move operation on handlers of 2 different kinds of pool types raises compile error (handler must be of same type first so that we can use them )
//same like you cant use move assignment on unique pointers of 2 different types
//eg - std::unique_ptr<int>intPtr-std::make_unique<int>(5); std::unique_ptr<double>doublePtr; doublePtr=std::move(intPtr)  -> compile error
//however you can move from a derived class to its base class (upcasting)

//making your own exception class derived from std::bad_alloc
//std::exception has a virtual member function named what() that returns a string of description of that exception 
class pool_exhausted: public std::bad_alloc{ //As of cpp 20,there are 28 different exception classes that can be thrown -> all these exception classes are derived from std::exception
public:
    [[nodiscard]] const char* what() const noexcept override{// returns a string literal => return type const char* , overriden from virtual what in std::exception
        return "ObjectPool: capacity exhausted";
    }
};
/* Explaination of pool_exhausted class-
    hirearchy is std::exception -> std::bad_alloc -> pool_exhausted
    exception and bad_alloc are classes defined in STL
    class exception{
    public:
        virtual const char* what() const noexcept;
        virtual ~exception();
    };

    what() basically returns a human-readable description of the exception 
    catch(const std::exception& error){
        std::cout<<error.what();
    }
    
    std::bad_alloc represents failure to obtain storage
    so we make a specific class pool_exhausted inherited from class bad_alloc defined in the STL of cpp (which is also inherited from class exception) 
    consequently callers can catch exceptions at different levels
    catch(const pool_exhausted& error){
        //specifically pool exhaustion
    }
    catch(const std::bad_alloc& error){
        //any allocation failure
    }
    catch(const std::exception& error){
        //any standard style exception 
    }


    Example usage- 
    try{
        pool.acquire();
    }
    catch(const std::exception& error){ ->reference binds to the derived pool_exhausted object and virtual dispatch calls pool_exhausted::what() [it is written for explaination, otherwise if you write pool_exhausted::what() inside code it will give compile error since what() is not a static fxn]
        //error is an object of base class excepttion and what() is a  non static function, so we are accessing it through the object 
        std::cout<<error.what()<<endl;
    }

*/

// Exhaustion policies - Each policy provide a 'static void on_exhausted'
//1) If it returns normally, acquire() yields a nullptr
//2) If it throws or terminates, acquire() never returns

//on_exhausted() function is static bcz then we don't have to make an object of the struct and then this function is associated with the struct itself instead of an object (static function can be called without an object)
//eg- NullOnExhaustion::on_exhausted()
struct ThrowOnExhaustion{
    [[noreturn]] static void on_exhausted(){ //[[noreturn]] tells compiler that a function never returns normally to its caller (helps compiler understand control flow, perform optimization, avoid missing return warnings)
        throw pool_exhausted{};  //equivalent to throw pool_exhausted() -> braces are generally preffered mordern cpp initialization syntax 
        //creates a temporary object of type pool_exhausted (explty curly braces mean construct it with no arguments)
        //the exception system stores the thrown aobject and then a matching catch can bind to it
        //catch(const pool_exhausted& error){std::cout<<error.what();} -> error is reference to the stored exception object , it is destoryed after the handler finishes 
    }
};
struct NullOnExhaustion{
    static void on_exhausted() noexcept{  

    }
};
struct AbortOnExhaustion{//pool exhaustion is considered unrecoverable, so application stops immediately 
    [[noreturn]] static void on_exhausted() noexcept{
        assert(false && "ObjectPool: capacity exhausted");
        std::abort();  //immediately terminates entire program abnormally then OS destroys process VA space 
    }
};

struct PoolStats{
    std::size_t capacity;
    std::size_t in_use;  //memory currently handed out
    std::size_t available; // available=capacity-in_use
    std::size_t high_water_mark; //maximum in_use ever observed
};



//A fixed-capacity pool of 'Capacity' slots for objects of type 'T'
//
//slots are raw storage;objects are constructed in place on acquire() & destoryed on release(). Both are O(1) with no allocation after construction 
//----Placement------------------------------------------------------------------------------
//The slab is an Embedded array, sizeof(ObjectPool) is roughly SlotSize*Capacity. Choose where the object pool lives accordingly:
//ObjectPool<Particle,64>p; //Object Pool lives on stack - fine while small
//static ObjectPool<Particle,100000>p;//.bss, no allocation of mememory ever
//thread_local ObjectPool<Job,1024>p;//per thread, lock free by design
//auto p=std::make_unique<ObjectPool<Particle,1000000>>();//one heap alloc
//Rule of thumb- Anything over ~100KB must not be a stack lkocal, default stack are 1MB on windows and 8MB on linux
//
//-----------Thread Safety----------------------------------------------------------------------
//Not thread safe. (bcz a mutex would cost more than the allocation it replaces)
//use one 'thread_local' pool per thread instead
//
//---------------------Exception-------------------------------------------------------------
//acquire() gives stronbg gurantee: If T's constructor throws, the pool is byte-for-byte unchanged and the exception propogates 

//we made ExhaustionPolicy a type instead of a bool or enum parameter bcz then user can define its own custom ExhaustionPolicy struct eg- struct LogAndThrow
template<typename T, std::size_t Capacity, typename ExhaustionPolicy=ThrowOnExhaustion>//Capacity is unsized as std::size_t is unsigned , default type is ThrowOnExhaustion struct if nothing is mentioned while making ObjectPool 
class ObjectPool{
public:
    ObjectPool() noexcept{//links every slot in one chain: slot 0->slot1-.....->nullptr ; complexity: O(capacity)
        Node* prev=nullptr;

        for(std::size_t i=Capacity;i>0;){  //had to write for loop like this bcz i is size_t variable, so when i=0 , i-- wraps over (it is unsigned)
            i--;
            prev= ::new(static_cast<void*>(slot_address(i))) Node{prev};
            /* This statement is equivalent to -
                std::byte* raw= slot_address(i);   
                void* mem=static_cast<void*>(raw);  // a static cast doesnt allocate, move or modify memeory , infact no cast is required at all coz an object ptr converts implicitly to void* (but for production level code,its better to write)
                Node* n= ::new (mem) Node{prev};  ->Node{prev} is aggregate initialization - it fills struct's memembers positionally from the braced list, Node{} //next=nullptr
                prev=n;
            */
        }
        free_head_=prev;
    }

    //acquire()
    //Constructs a T in free slot and returns a poiner to it; returns a nullptr when pool is exhausted
 
    //Args are perfectly forwarded to T's constructor (exactly like emplace back in vector)- The pool never copies or moves the object| Acquire is noexcept iff both arguments inside the noexcept evaluates to true
    template<typename... Args>
    [[nodiscard]] T* acquire(Args&&... args) noexcept(std::is_nothrow_constructible_v<T,Args...> && //this is a compile time boolean (part of <type_traits>)- evaluates to true if T can be constructed from Args... without throwing (if constructor is noexcept=> true, otherwise false) | In is_nothrow_constructible_v , the _v is a C++17 helper(stands for value),_v extracts boolean value directly
                                                    noexcept(ExhaustionPolicy::on_exhausted()))  //noexcept(true) means function doest throw. For NullOnExhaustion  and AbortOnExhaustion it evaluates to true (on_exhausted is noexcept for these 2 exhaustion types)
    {//Args&& is a forwarding reference[NOT A rvalue reference] (w/o perfect forwarding every argument inside constructor of T will arrive as lvalue even if u sent rvalue); acquire() works even if u pass fully constructed particle onject in it instead of just constructor args (it then uses T's copy constructor to construct another object inside slot)
        if(free_head_==nullptr){
            ExhaustionPolicy::on_exhausted();//calls the function inside the struct/class ExhaustionPolicy

            return nullptr; //couldn't acquire 
            /*Eg usage- objpool::ObjectPool<Particle,2,objpool::NullOnExhaustion>p2;
            auto* a=p2.acquire(1.0f,2.0f);
            auto *b=p2.acquire(5.0f,6.0f);//f=nullptr, check it 
            */
        }
        Node* slot=free_head_;
        Node* next=slot->next;

        //if T's constructor throws exception then although free_head_ will remain intact but memory that free_head_ is pointing to will get polluted (bcz maybe T's constructor wrote some 12 bytes before throwng)
        //hence free_head_->next wont work properly

        T* obj;
        try{//Note- try catch block has no overhead when nothing throws bcz exception handling is table driven(unwind info lives in seperate section;happy path emits zero extra instruction)
            obj=::new(static_cast<void*>(slot)) T(std::forward<Args>(args)...); //object's lifetime ends when its storage is used (so Node object lifetime ends here)
            //moreover we dont have to call destructor first in this case bcz Node is trivially destructable object (class uses default destructor )-> Reusing its storage without destroying is free and legal
            // for non trivally destructible object , we should destroy it frist before reusing its stroage , otherwise it will cause memory leak (though functionally it will work correctly)
        }
        catch(...){//catch all handler
            //T's constructor may have partially overriteen Node::next (since before acquire this memory was occupied with the object of class Node which had a 8 byte pointer )
            //rebuild the Node object from our cached copy so free list is byte-for-byte what it was on entry, then let the exception fly
            ::new(static_cast<void*>(slot)) Node{next};//Node{next} is basically constructing a Node object with the this->next=next (basically pointing to next node)
            throw;//Bare throw rethrows the origional exception object (preserving )

            //Note- we dont call ~T() bcz if T's constructor throws, compiler automatically destorys every fully constructed base class and member subobject before exception leaves
            //our only job is restoring our own invariant: Node
            //Note- In general objects constructor (T's constructor) have no try catch block at all, they only throw exception (i.e have only throw keyword) - propogating is their default behaviour
            //In case T's constructor has a try catch block and T recovers interally then => constructor completes successfully - no exception ever reaches us (everything executes normally)
            //When T's constructor only throws our catch block doesnt complete normally - it rethrows and the exception continues unwinding out of aquire
            //(normally a catch block that completes normally lets execution continue after that - but since our ends with throw free_head_=next; and return obj; will never execute, they are skipped bcz of throw)
            //we passed the callers error to the catch block of acquire() thats why acquire() is not a noexcept function 
        
            
        }
        free_head_=next;//commit only on success i.e when slot has been successfully allocated to the object T
        ++in_use_;
        high_water_mark_=std::max(high_water_mark_,in_use_);
        return obj;

        //TLDR- our catch(...) runs whenever an exception escapes T's constructor (default behaviours of constructor) . throw; rethrows so the commit lines never execute. Our only  job is restoring Node
    }
    
    void release(T* obj) noexcept{//noexcept bcz 1)~T() is implicitly noexcept unless explicitly marked otherwise; 2)Placement new of trivial aggregate can't throw exception; 3) Last step is just pointer assignment
        if(obj==nullptr)return;
        
        assert(owns(obj) && "ObjectPool: release: pointer does not belong to this pool"); //if first condition fails then only the thing on right of && executes, string is included so the failed assertion output contains a useful explaination 
        assert(!is_in_free_list(obj) && "ObjectPool: release: double release detected");// O(N) check
        //assert only works during debug build, in release build (-O2 -DNDEBUG) assert statements are removed (it expands to ((void)0))
        // Asserts encode preconditions — conditions a correct program can never violate — so they're compiled out with  NDEBUG  and cost nothing in production. 
        //Runtime conditions like exhaustion get a real exception instead. And the double-release assert is only a backstop for the raw API; the RAII handle makes that bug unrepresentable rather than merely detected
        
        std::destroy_at(obj);//run ~T() while the storage still holds a valid T (since T may hold memory, locks or file handls)- destroy_at() explicitly destroys an object at a given address
        //bytes are raw again ,=> build a node here linked to current head. LIFO -> slot goes to front
        //destory_at() doesn't erase anything-it ends a lifetime, std::destroy_at(obj) is obj->~T()
        //Note- destroy_at(obj) performs no ownership or lifetime checks - it assumes obj points to a live T (a second release will be UB bcz the slot will contain a Node not a live T)

        Node* slot=::new(static_cast<void*>(obj)) Node{free_head_};//obj pointer of type T* still exists its just that object at that pointer is destroyed, its just raw memory
        //Node with next=free_head_ is constructed 
        free_head_=slot;
        --in_use_;
        //even if the slot is released from middle then also in the intrusive free list, we make that slot as the first memember (so that when it calls aquire, that memory will be sitting in L1 cache)

    }
    ~ObjectPool() = default; //if destructor is defined, implicity move operations are suppressed (compiler automaticaly doesn't generate move constructor if destructor is present)

    //A pool can neither be copied nor be moved
    //free_head_ and Node::next holds absolute address in the storage_ array, which is embedded on this object
    //copying or moving leaves all these pointers aimed at the source pool's slab and that will be a disaster (since the array will get copied at some other address but pointer will point to same thing)
    //move could have been implemented if storage_ would have been a dynamically allocated array (std::byte* storage_) 
    //since we have embedded array move would be a mess, and copying will require rebasing the address of free_head_ (which is difficult)

    ObjectPool(const ObjectPool&)=delete;
    ObjectPool& operator=(const ObjectPool&) =delete;
    ObjectPool(ObjectPool&&)=delete;
    ObjectPool& operator=(ObjectPool&&) =delete;
    //We can't make a vector of ObjectPool now bcz vector requires move or copy (when size=capacity) -> workaround allocate objectPool on heap and make vector of pointers of object pool


    [[nodiscard]] static constexpr std::size_t capacity() noexcept{  //capacity is the class specific property (hence static), different objects of same class have same capacity - determined at compile time - Also static member functions can' be const
        return Capacity;//changing Capacity will give a compile error (neither can someone use &Capacity) - Capacity is non-type template parameter, its a prvalue constant 
        //changing Capacity is rubbish bcz there is no storage, no address ,no lvalue - its like 4=8, capacity is a part of type identitry ObjectPool<Particle,4>a;
    }
    [[nodiscard]] std::size_t size() const noexcept{
        return in_use_;
    }
    [[nodiscard]] std::size_t available() const noexcept{
        return Capacity-in_use_;
    }
    [[nodiscard]] bool exhausted() const noexcept{
        return free_head_==nullptr;
    }
    [[nodiscard]] std::size_t high_water_mark() const noexcept{
        return high_water_mark_;
    }
    [[nodiscard]] PoolStats stats() const noexcept{
        return PoolStats{Capacity,in_use_,Capacity-in_use_,high_water_mark_};
    }

    //Debug aid- is 'p' the address of a slot in this pool ? (checks range and slot alignment , O(1), proves a pointer is foreign, never that it is currently live)
    [[nodiscard]] bool owns(const T* p) const noexcept{
        const auto addr=reinterpret_cast<std::uintptr_t>(p);   //uintptr_t is an unsigned integer type capable of holding a pointer value ,whereas p has a type const T* , addr recieves an integer representation  of address stored in p 
        //reinterpret_cast is needed bcz ordinary implicit conversion and static_cast don't convert pointers to integer , static_cast<std::unintptr_t>(p) gives error
        //Dont care about how it converts, but this conversion is needed bcz we perform integer airthmetic 
        //  addr-storage_ when they are not in same array is an UB  (pointer subtraction is defined as distance in elements within one array, p-q is valid onluy when both point to same array object) 
        const auto begin=reinterpret_cast<std::uintptr_t>(storage_);
        const auto end=begin+SlotSize*Capacity;
        return addr>=begin && addr<end && ((addr-begin)%SlotSize)==0;
    }

private:
    //storage+ free list go here
    static_assert(Capacity>0, "Pool capacity must be greater than 0");//compile time assertion
    
    //Create Raw, uninitialized storage for Capacity number of objects of type T
    //Not doing T objects_[Capacity] as it will then become a free list of pre constructed objects , drawbacks:  Can't construct inplace (emplace back iden), doesnt compile if the object has no default constructor
    // alignas(alignof(T)) std::byte storage_[sizeof(T)*Capacity];  //alignas: gurantees the slab begins at an adress satisfying T's alignment requirement, so every slot boundary does too. 
    //misaligned addresses is an undefined behaviour in cpp and std::byte [] only gurantees alignment 1
    //since slab starts T aligned and each slot is sizeof(T) bytes apart, every slot is automatically T aligned too! (bcz sizeof(T) is always a multiple of alignof(T)-returns alignment requirement of T )
    
    
    //the address of slot i is strorage_+i*sizeof(T)
    //below function returns address of slot i
    // [[nodiscard]] std::byte* slot_address(std::size_t i) noexcept{  //nodiscard means"Calling this function only makes sense if you use the returned address", this function returns address (byte*), so if something like slot_address(2) is used (instead of auto* addr=slot_address(2)) compiler raises warning
    //     return storage_ + i*sizeof(T);
    // }

    //A slot's bytes hold either a live T(occpied ) or a Node (i.e the slot is free)  [raw memory me kuch toh object hona chaiye na,slot me raw bytes hai ,Node is an object which holds pointer to next free node]
    //Never both, bbcz a free slot has no object in it. The free list costs no mremory beyond the slab itself. 
    struct Node{  //Node's class objects are trivially destructible [node has no user defined destructor and uses default destructor by compiler] 
        Node* next;
    };  //sizeof(Node)=size of ptr on machine = 8 byte on 64 bit machine
    //Problem 1 - sizeof(Node)<sizeof(T) {say T is char} . Fix: size of slot must be atleast sizeof(Node)
    //Problem 2- T's alignment maybe weaker than Node's (say T is char ,alignof(T)=1, while alignof(Node)=8)  Fix: Align the slab to max(alignof(T), alignof(Node)) [Note- in case of powers of 2 , LCM is same as max]
    //Problem 3-  Slab/slot size should be a multiple of its alignment =max(alignof(T),alignof(Node)) say T= struct Tag{char data[9]}; alignof(T)=1; sizeof(T)=9; alignof(slab)=8; sizeof(slab)=max(sizeof(T),sizeof(Node)) {since Slab holds either T or Node}->round up to be multiple of align of

    static constexpr std::size_t SlotAlign=std::max(alignof(T),alignof(Node));  //LCM for powers of 2 is same as max
    static constexpr std::size_t RawSlotSize=std::max(sizeof(T),sizeof(Node));

    ///round raw slot size to multiple of SlotAlign  (for reasoons mentioned above)
    static constexpr std::size_t SlotSize=((RawSlotSize+SlotAlign-1)/SlotAlign)*SlotAlign;  //result=((x+a-1)/a)*a rounds x up to multiple of a , logic: x=qa+r , if r=0 then result=qa, if(r>0)adding a-1 ,x+a-1=(q+1)*a+r-1, 0<=r-1<=a-1 => result=(q+1)a

    //static_assert evaluates at compile time (condition must be compile time constant, gives compile error if it fails whereas normal assert calls std::abort giving RE), normal assert evaluates at runtime (so basically everything that evaluates at comnpile time doesnt add to the latency of our code)
    static_assert(Capacity<=SIZE_MAX/SlotSize, "Capacity too large: SlotSize*Capacity overflows std::size_t (A negative capacity wraps to a huge value-check for that) ");//SIZE_MAX is standard macro representing max value of std::size_t (2^64-1 for 64 bit system)
    static_assert(std::is_trivially_destructible_v<Node>, "Node must be trivially destructible: acquire() resues its storage without running a destructor ");//Trivially destructible objects have no custom destructor and no virtual destructor & trivial members , ~MyClass()=default, compiler automatically destructs and destruction requires no complex logic
    //if Node is not trivially destructible then it might cause memory leak in acquire() bcz we dont call std::destory_at(slot) in acquire, also our release is noexcept bcz Node is trivial (complex node might throw)
    //keeping Node trivial helps us to avoid unnecessarily increasing overhead and latency . Suppose Node is complex and has a string, so every time during construction and destruction it will involve allocating heap/deallocating heap -> unnecessary latency spike
   
   
    alignas(SlotAlign) std::byte storage_[SlotSize*Capacity];
    //now we have a slab of raw bytes with fized sized slots; we want a way to answer give me a free slot in O(1)
    Node* free_head_{nullptr};//Head of the intrusive free list, nullptr means the pool is exhausted 
    std::size_t in_use_{0};
    std::size_t high_water_mark_{0};

    [[nodiscard]] std::byte* slot_address(std::size_t i) noexcept{//returns address of slot number i 
        return storage_ + i*SlotSize;
    }
    //checks if pointer p is present in the free list
    [[nodiscard]] bool is_in_free_list(const void* p) const noexcept{//we only care about address, not the object type so thats why const void* , p can be of T* or Node* (in case we are about to double release)
        for(const Node* curr=free_head_;curr!=nullptr;curr=curr->next){
            if(static_cast<const void*>(curr)==p)return true;
        }
        return false;
    }
     
};


}




















































































































































































































































































































































































































































































































































































































































































































































































































































































































































































































































































































































































































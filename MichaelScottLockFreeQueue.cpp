//ported line-by-line from psuedocode in the original paper from 1996 http://www.research.ibm.com/people/m/michael/podc-1996.pdf
//comments in the enqueue/dequeue functions are from the paper as well

//http://en.wikipedia.org/wiki/Non-blocking_algorithm - parallel programming without locks

//explanation of what atomic_compare_exchange_strong does (the CAS operation): http://en.wikipedia.org/wiki/Compare-and-swap 
//(note: atomic_compare_exchange_weak would be more performant, but prone to "spurious failure." Also note that std::atomic implements these as member functions as well)

//A C++ production-level implementation of this algorithm in Boost: http://www.boost.org/doc/libs/1_53_0/boost/lockfree/queue.hpp
//In 2011, Kogan and Petrank expanded on this algorithm to create a ground-breaking "wait-free" queue algorithm (no arbitrarily repeating wait-loops like the while(true) ones you see here)

#include<iostream>
#include<atomic>
#include<memory>
#include<type_traits>
#include<future>
#include<thread>
#include<array>
#include<chrono>

using std::atomic;
using std::atomic_compare_exchange_strong;
using std::is_trivially_copyable;
using std::future;
using std::async;
using std::array;
using std::this_thread::sleep_for;
using std::chrono::microseconds;
using std::cout;
using std::cin;
using std::endl;


template<typename T>
struct NodePointer;

template<typename T>
struct Node{

	static_assert(is_trivially_copyable<T>::value, "T in Node<T> must be trivially copyable.");

	T value;
	atomic<NodePointer<T>> next;

	Node(){}
	Node(NodePointer<T> np, T val) :next(np), value(val) {}
};

//the NodePointer struct allows us to combine a pointer with a count - this solves what is known as the "ABA problem" http://en.wikipedia.org/wiki/ABA_problem
template<typename T>
struct NodePointer {
	Node<T>* ptr;
	int count;

	NodePointer(Node<T>* p, int c) :ptr(p), count(c){}
	NodePointer() :ptr(nullptr){}

	NodePointer<T>& operator=(Node<T>*p){
		ptr = p;
		return *this;
	}
};

template<typename T>
static inline bool operator==(const NodePointer<T>& a, const NodePointer<T>& b){
	return a.ptr == b.ptr && a.count == b.count;
}

template<typename T>
class Queue {

	//std::atomic lets us load() and store(x) to and from an object in a single operation, guaranteed to finish completely
	atomic<NodePointer<T>> _head;
	atomic<NodePointer<T>> _tail;

public:

	Queue(){
		auto node = new Node<T>();
		auto ptr = NodePointer<T>(node, 0);

		// Make a new node the only node in the linked list. Both _head and _tail point to it
		_head.store(ptr);
		_tail.store(ptr);
	}

	Queue& enqueue(T val){

		//Copy enqueued value into new node, with null next
		auto node = new Node<T>(NodePointer<T>(nullptr, 0), val); 
		NodePointer<T> tail;

		//Keep trying until enqueue is done (loops like this are what qualify this algorithm as lock-free but not wait-free)
		while (true){ 

			//load the head and tail from memory at this exact point in time
			tail = _tail.load(); 
			auto next = tail.ptr->next.load();

			//Are tail and next consistent?
			if (tail == _tail.load()){

				//Was _tail pointing to the last node?
				if (next.ptr == nullptr){

					//Try to link node at the end of the linked list
					if (atomic_compare_exchange_strong(&tail.ptr->next, &next, NodePointer<T>(node, next.count + 1))){
						//enqueue is done. Exit loop
						break;
					}

				}
				else{
					//_tail was not pointing to the last node, so try to swing _tail to the next node
					atomic_compare_exchange_strong(&_tail, &tail, NodePointer<T>(next.ptr, tail.count + 1));
				}
			}
		}

		//Enqueue is done. Try to swing _tail to the inserted node
		atomic_compare_exchange_strong(&_tail, &tail, NodePointer<T>(node, tail.count + 1)); 


		return *this;
	}

	bool dequeue(T& result)
	{
		NodePointer<T> head;

		//Keep trying until dequeue is done
		while (true){

			head = _head.load(); 
			auto tail = _tail.load(); 
			auto next = head.ptr->next.load();

			//Are head, tail, and next consistent?
			if (head == _head.load()){ 

				//Is queue empty or _tail falling behind?
				if (head.ptr == tail.ptr){ 

					// Is queue empty?
					if (next.ptr == nullptr){ 
						// Queue is empty, couldn't dequeue
						return false; 
					}

					//_tail is falling behind. Try to advance it
					atomic_compare_exchange_strong(&_tail, &tail, NodePointer<T>(next.ptr, tail.count + 1)); 
				}

				else{
					//No need to deal with _tail
					//Read value before CAS, otherwise another dequeue might free the next node
					result = next.ptr->value;

					//Try to swing _head to the next node
					if (atomic_compare_exchange_strong(&_head, &head, NodePointer<T>(next.ptr, head.count + 1))){ 
						//Dequeue is done. Exit loop
						break;
					}

				}
			}
		}

		//It is safe now to free the old dummy node
		delete head.ptr; 

		//Queue was not empty, dequeue succeeded
		return true; 
	}
};

//now let's try it out...
const int THREAD_COUNT = 10;
const int RESULT_COUNT = 100;

int main(){

	auto q = Queue<int>();
	auto counter = 0;
	auto results = Queue<int>();

	//spawn several threads racing to enqueue in and dequeue from the same queue at the same time
	array<future<void>, THREAD_COUNT> futures;
	
	for (auto i = 0; i < THREAD_COUNT; ++i){

		futures[i] = async([&](){

			int result = 0;

			while (counter < RESULT_COUNT){

				q.enqueue(counter++);

				while (counter % 2 != 0 && q.dequeue(result)){

					results.enqueue(result);

					//sleeping the threads is totally unnecessary; it just tends to mix up the order of the results a bit
					sleep_for(microseconds(10));

				}

				sleep_for(microseconds(10));
			}
		});
	}

	//wait on threads to complete
	for (auto i = 0; i < THREAD_COUNT; ++i)	{

		futures[i].get();

	}

	auto numberToPrint = 0;

	while (results.dequeue(numberToPrint))	{

		cout << numberToPrint << endl;

	}

	cout << "leftovers from data race:" << endl;

	while (q.dequeue(numberToPrint)){
		cout << numberToPrint << endl;
	}

	cin.get();
	return 0;
}


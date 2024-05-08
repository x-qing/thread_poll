#include "threadpool.h"
#include <functional>
#include <thread>
#include <iostream>
 
const int TASK_MAX_THREADHOLD = INT32_MAX;
const int THREAD_MAX_THREADHOLD = 1024;
const int THREAD_MAX_IDLE_TIME = 60;   //单位是s

// 构造函数
ThreadPool::ThreadPool() :
	initThreadSize_(0),
	taskSize_(0),
	taskQueMaxThreadHold_(TASK_MAX_THREADHOLD),
	poolMode_(PoolMode::MODE_FIXED),
	isPoolRunning_(false),
	idleThreadSize_(0),
	threadSizeThreadHold_(THREAD_MAX_THREADHOLD),
	curThreadSize_(0)
{}


// 析构函数
ThreadPool::~ThreadPool() {
	isPoolRunning_ = false;
	//notEmpty_.notify_all();
	// 等待线程池里面所有的线程返回  有两种状态：阻塞 & 正在执行任务中
	std::unique_lock<std::mutex> lock(taskQueMtx_);
	notEmpty_.notify_all();
	/* Pool的析构函数一直阻塞在这里，认为此时的size还不为0，认为线程队列里面还有一个线程*/
	exitCond_.wait(lock, [&]() { return threads_.size() == 0; });
}


// 设置线程池的工作模式
void ThreadPool::setMode(PoolMode mode) {
	if (checkRunningState()) {
		return;
	}
	poolMode_ = mode;
}

// 设置task任务队列上限的阈值
void ThreadPool::setTaskQueMaxThreadHold(int threadhold) {
	if (checkRunningState()) {
		return;
	}
	taskQueMaxThreadHold_ = threadhold;
}

// 给线程池提交任务
// 用户调用该接口，传入任务对象，生产对象
Result ThreadPool::submitTask(std::shared_ptr<Task> sp) {
	// 获取锁
	std::unique_lock<std::mutex> lock(taskQueMtx_);
	// 线程的通信 等待任务队列有空余
	//while (taskQue_.size() == taskQueMaxThreadHold_) {
	//	notFull_.wait(lock);  // 进入等待状态
	//}
	if (!notFull_.wait_for(lock, std::chrono::seconds(1),
		[&]()->bool {return taskQue_.size() < (size_t)taskQueMaxThreadHold_; })) {
		// 表示notFull条件变量等待1s，条件依然没有满足
		std::cerr << " task queue id full,submit task fail." << std::endl;
		//return task->getResult(); 行不通，线程执行完task，task对象就被析构了
		return Result(sp,false); // Task Reslut
	}

	// 如果有空余，把任务放入任务队列中
	taskQue_.emplace(sp);
	taskSize_++;
	// 因为新放了任务，任务队列肯定不空了，在notEmpyt_ 通知线程执行任务
	notEmpty_.notify_all();

	// cached模式 需要根据任务数量和空闲线程的数量，判断是否需要创建新的线程？
	// 处理比较紧急的任务，场景：小而块的任务
	if (poolMode_ == PoolMode::MODE_CACHED && taskSize_ > idleThreadSize_ &&
		curThreadSize_ < threadSizeThreadHold_) {
		std::cout << ">>>>>>>>> create new thread..." << std::endl;
		// 创建新的线程
		auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this,std::placeholders::_1));
		int threadId = ptr->getId();
		threads_.emplace(threadId, std::move(ptr));
		// 启动线程
		threads_[threadId]->start();
		// 修改线程个数相关的变量
		curThreadSize_++;
		idleThreadSize_++;   // 刚起来的线程为空闲线程
		//threads_.emplace_back(std::move(ptr));
	}

	// 返回任务的Result对象
	return Result(sp);
}

// 开启线程池
void ThreadPool::start(int initThreadSize) {
	
	// 设置线程池的运行状态
	isPoolRunning_ = true;
	// 记录初始线程的个数
	initThreadSize_ = initThreadSize;
	curThreadSize_ = initThreadSize;

	// 创建线程对象
	for (int i = 0; i < initThreadSize_; i++) {
		// 绑定，这样Thread对象的start方法就可以调用 ThreadPool的threadFunc方法了
		//threads_.emplace_back(new Thread(std::bind(&ThreadPool::threadFunc,this)));
		auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this,std::placeholders::_1));
		int threadId = ptr->getId();
		threads_.emplace(threadId, std::move(ptr));
		//threads_.emplace_back(std::move(ptr));
	}

	// 启动所有线程
	for (int i = 0; i < initThreadSize_; i++) {
		threads_[i]->start();   // 需要执行一个线程函数
		idleThreadSize_++;   // 记录初始空闲线程的数量
	}
}

// 这样函数访问线程池中的函数就非常的发方便
// 线程池的所有线程从任务队列里面消费任务
void ThreadPool::threadFunc(int threadid) {
	//std::cout << "begin threadFunc tid: " << std::this_thread::get_id() <<  std::endl;
	//std::cout << "end threadFunc tid :" << std::this_thread::get_id() << std::endl;

	auto lastTime = std::chrono::high_resolution_clock().now();


	// 所有任务必须执行完成，线程池才可以回收所有的线程资源
	// while (isPoolRunning_)
	for (;;)  {
		std::shared_ptr<Task> task;
		{
			// 获取锁
			std::unique_lock<std::mutex> lock(taskQueMtx_);

			std::cout << "tid: " << std::this_thread::get_id() << "尝试获取任务..." << std::endl;

			// cached模式下，有可能已经创建了很多线程，但是空闲事件超过60s，应该把多余的线程回收掉
			// 超过initThreadsize_的线程要进行回收
			// 当前时间 - 上一次线程执行的时间 > 60
			// 每一秒钟返回一次
			// 怎么区分是超时返回还是有任务返回

			// 锁+双重判断
			while (isPoolRunning_ && taskQue_.size() == 0){
				if(poolMode_ == PoolMode::MODE_CACHED){
					// 条件变量超时返回
					if (std::cv_status::timeout == notEmpty_.wait_for(lock, std::chrono::seconds(1))) {
						auto now = std::chrono::high_resolution_clock().now();
						auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
						if (dur.count() >= 60 && curThreadSize_ > initThreadSize_) {
							// 回收当前线程
							// 记录线程数量的相关变量的值
							// 把线程对象从线程列表容器中删除
							// 没有办法threadFunc和Thread对象
							// threadid -> thread对象->删除
							threads_.erase(threadid);   // 不要使用std::this_thread::getid()
							curThreadSize_--;
							idleThreadSize_--;
							std::cout << "threadid:" << std::this_thread::get_id() << "exit!" << std::endl;
							return;
						}
					}
				}
				else {
					// 等待notEmpty条件
					notEmpty_.wait(lock);
				}

				// 等待的线程被唤醒后，变为阻塞，获取锁之后起来，发现isPoolRunning_为false了。那么从线程中删除
				// 线程池要结束，回收线程资源
				//if (!isPoolRunning_) {
				//	threads_.erase(threadid);   // 不要使用std::this_thread::getid()
				//	std::cout << "threadid:" << std::this_thread::get_id() << "exit!" << std::endl;
				//	exitCond_.notify_all();
				//	return;
				//}
			}

			if (!isPoolRunning_) {
				break;
			}
			idleThreadSize_--;

			std::cout << "tid: " << std::this_thread::get_id() << "获取任务成功..." << std::endl;
			// 取一个任务
			task = taskQue_.front();
			taskQue_.pop();
			taskSize_--;
		
			// 如果依然有剩余任务，继续通知其他的线程执行任务
			if (taskQue_.size() > 0) {
				notEmpty_.notify_all();
			}

			// 取出一个任务进行通知,通知可以继续提交生产任务

			notFull_.notify_all();
		}// 释放锁

		// 当前线程负责执行这个任务
		if (task != nullptr) {
			//task->run();   // 执行任务，把任务的返回值setVal方法给到Result
			task->exec();
		}
		idleThreadSize_++;
		// 更新线程执行完任务的时间
		auto lastTime = std::chrono::high_resolution_clock().now();
	}
	// 两种线程的情况，
	// 一种：原来的线程就阻塞着
	// 另一种：线程正在执行任务
	// 
	threads_.erase(threadid);   // 不要使用std::this_thread::getid()
	std::cout << "threadid:" << std::this_thread::get_id() << "exit!" << std::endl;
	exitCond_.notify_all();
}

bool ThreadPool::checkRunningState() const {
	return isPoolRunning_;
}


void ThreadPool::setThreadSizeThreadHold(int threadhold) {
	if (checkRunningState())
		return;
	if (poolMode_ == PoolMode::MODE_CACHED) {
		threadSizeThreadHold_ = threadhold;
	}
}


/////////////////////  线程方式实现  /////////////////////

int Thread::generateId_ = 0;

// 线程构造
Thread::Thread(ThreadFunc func) :func_(func),threadId_(generateId_++) {}
// 线程析构
Thread::~Thread() {}

// 启动线程
void Thread::start() {
	// 创建一个线程来执行一个线程函数
	std::thread t(func_,threadId_);			// 线程对象t和线程函数func_
	t.detach();		 // 设置分离函数
}

int Thread::getId() const {
	return threadId_;
}

////////////////////////// Task方法实现   //////////////////////////////
Task::Task():result_(nullptr){}

void Task::exec() {

	if (result_ != nullptr) {
		result_->setVal(run());   // 这里发生多态调用
	}
}

void Task::setResult(Result* res) {
	result_ = res;
}

///////////////////////////Result方法的实现/////////////////////
Result::Result(std::shared_ptr<Task> task, bool isvalid):isValid_(isvalid),task_(task) {
	task_->setResult(this);
}


// 用户调用的
Any Result::get() {
	if (!isValid_) {
		return "";
	}
	sem_.wait();   // task任务还没有执行完，这里阻塞用户的线程
	return std::move(any_);
}

// 
void Result::setVal(Any any) {
	// 存储task的返回值
	this->any_ = std::move(any);
	sem_.post();   // 已经获取了任务的返回值，增加信号量资源

}
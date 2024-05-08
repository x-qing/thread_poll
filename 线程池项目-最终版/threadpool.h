#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <vector>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>    // 条件变量
#include <functional>
#include <unordered_map>
#include <thread>
#include <future>
#include <iostream>


const int TASK_MAX_THREADHOLD = 2; //  INT32_MAX;
const int THREAD_MAX_THREADHOLD = 1024;
const int THREAD_MAX_IDLE_TIME = 60;   //单位是s

// 加上class避免枚举名字不一样，但是里面的项是一样的
// 线程池支持的两种模式
enum class PoolMode {
	MODE_FIXED,     // 固定数量的线程
	MODE_CACHED	// 线程数量可动态增长
};


// 线程类型
class Thread {
public:
	// 线程函数对象类型
	using ThreadFunc = std::function<void(int)>;

	// 线程构造
	Thread(ThreadFunc func) :func_(func), threadId_(generateId_++) {}
	// 线程析构
	~Thread() = default;
	// 启动线程
	void start() {
		// 创建一个线程来执行一个线程函数
		std::thread t(func_, threadId_);			// 线程对象t和线程函数func_
		t.detach();		 // 设置分离函数
	}

	// 获取线程id
	int getId() const {
		return threadId_;
	}
private:
	ThreadFunc func_;   // 函数对象
	static int generateId_;
	int threadId_;	    // 保存线程id
};


int Thread::generateId_ = 0;

/*
* example:
* ThreadPool pool;
* pool.start(4);
* class Mytask:public Task{
*	public:
*		void run() {线程代码};
* }
* pool.submitTask(std::make_shared<MyTask>());
*
*/
// 线程池类型
class ThreadPool {
public:

	// 线程池构造
	// 构造函数
	ThreadPool() :
		initThreadSize_(0),
		taskSize_(0),
		taskQueMaxThreadHold_(TASK_MAX_THREADHOLD),
		poolMode_(PoolMode::MODE_FIXED),
		isPoolRunning_(false),
		idleThreadSize_(0),
		threadSizeThreadHold_(THREAD_MAX_THREADHOLD),
		curThreadSize_(0)
	{}

	// 线程池析构
	~ThreadPool() {
		isPoolRunning_ = false;
		//notEmpty_.notify_all();
		// 等待线程池里面所有的线程返回  有两种状态：阻塞 & 正在执行任务中
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		notEmpty_.notify_all();
		/* Pool的析构函数一直阻塞在这里，认为此时的size还不为0，认为线程队列里面还有一个线程*/
		exitCond_.wait(lock, [&]() { return threads_.size() == 0; });
	}

	// 设置线程池的工作模式
	void setMode(PoolMode mode) {
		if (checkRunningState()) {
			return;
		}
		poolMode_ = mode;
	}

	// 设置task任务队列上限的阈值
	void setTaskQueMaxThreadHold(int threadhold) {
		if (checkRunningState()) {
			return;
		}
		taskQueMaxThreadHold_ = threadhold;
	}

	// 设置线程池cached模式下线程的阈值
	void setThreadSizeThreadHold(int threadhold) {
		if (checkRunningState())
			return;
		if (poolMode_ == PoolMode::MODE_CACHED) {
			threadSizeThreadHold_ = threadhold;
		}
	}

	// 给线程池提交任务
	// 使用可变惨模板编程，让submitTask可以接收任意函数和任意数量的参数
	template <typename Func,typename... Args>
	auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))> {
		// 打包任务，放入任务队列
		using RType = decltype(func(args...));   // 推导出来的是一个类型
		auto task = std::make_shared<std::packaged_task<RType()>>(
			std::bind(std::forward<Func>(func), std::forward<Args>(args)...)
			);
		std::future<RType> result = task->get_future();

		// 获取锁
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		// 线程的通信 等待任务队列有空余
		if (!notFull_.wait_for(lock, std::chrono::seconds(1),
			[&]()->bool {return taskQue_.size() < (size_t)taskQueMaxThreadHold_; })) {
			// 表示notFull条件变量等待1s，条件依然没有满足
			std::cerr << " task queue id full,submit task fail." << std::endl;
			auto task = std::make_shared<std::packaged_task<RType()>>(
				[]()->RType {return RType(); });
			(* task)();
			return task->get_future();
		}

		// 如果有空余，把任务放入任务队列中
		//taskQue_.emplace(sp);
		taskQue_.emplace([task]() {
			// 去执下面的任务
			(*task)();
			});
		taskSize_++;
		// 因为新放了任务，任务队列肯定不空了，在notEmpyt_ 通知线程执行任务
		notEmpty_.notify_all();

		// cached模式 需要根据任务数量和空闲线程的数量，判断是否需要创建新的线程？
		// 处理比较紧急的任务，场景：小而块的任务
		if (poolMode_ == PoolMode::MODE_CACHED && taskSize_ > idleThreadSize_ &&
			curThreadSize_ < threadSizeThreadHold_) {
			std::cout << ">>>>>>>>> create new thread..." << std::endl;
			// 创建新的线程
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
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
		return result;
	}

	// 开启线程池   // 当前系统cpu的核心数量
	void start(int initThreadSize = std::thread::hardware_concurrency()) {
		// 设置线程池的运行状态
		isPoolRunning_ = true;
		// 记录初始线程的个数
		initThreadSize_ = initThreadSize;
		curThreadSize_ = initThreadSize;

		// 创建线程对象
		for (int i = 0; i < initThreadSize_; i++) {
			// 绑定，这样Thread对象的start方法就可以调用 ThreadPool的threadFunc方法了
			//threads_.emplace_back(new Thread(std::bind(&ThreadPool::threadFunc,this)));
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
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

	// 禁止拷贝构造和赋值构造
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

private:
	// 定义线程函数
	void threadFunc(int threadid) {
		auto lastTime = std::chrono::high_resolution_clock().now();


		// 所有任务必须执行完成，线程池才可以回收所有的线程资源
		// while (isPoolRunning_)
		for (;;) {
			//std::shared_ptr<Task> task;
			Task task;
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
				while (isPoolRunning_ && taskQue_.size() == 0) {
					if (poolMode_ == PoolMode::MODE_CACHED) {
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
				task();   // 执行function<void()>的任务
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

	// 检查pool的运行状态
	bool checkRunningState() const {
		return isPoolRunning_;
	}

private:
	//std::vector<std::unique_ptr<Thread>> threads_;   // 线程列表
	std::unordered_map<int, std::unique_ptr<Thread>> threads_;
	int initThreadSize_;			 // 初始的线程数量
	int threadSizeThreadHold_;        // 线程数量上限的阈值
	std::atomic_int curThreadSize_;  // 记录当前线程池里面线程的总数量
	std::atomic_int idleThreadSize_;	// 记录空闲线程的数量

	// 直接用Task* 容易造成局部指针的问题,用户传入的Task的声明周期可能非常的短
	// 使用智能指针来拉长对象的声明周期，同时自动析构对象
	//std::queue<Task*> 

	// Task任务 == 函数对象
	using Task = std::function<void()>;
	std::queue<Task> taskQue_;        // 任务队列
	std::atomic_int taskSize_;        // 任务的数量
	int taskQueMaxThreadHold_;        // 任务队列上限的阈值

	std::mutex taskQueMtx_;       // 保证任务队列的线程安全
	std::condition_variable notFull_;        // 表示任务队列不满
	std::condition_variable notEmpty_;        // 表示任务队列不空

	std::condition_variable exitCond_;    // 等待线程资源全部回收

	PoolMode poolMode_;       // 当前线程池的工作模式

	// 表示当前线程池的启动状态
	std::atomic_bool isPoolRunning_;

};


// 


#endif // !THREADPOOL

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

// Any类型：可以接收任意数据的类型
class Any {
public:
	Any() = default;
	~Any() = default;
	Any(const Any&) = delete;
	Any& operator=(const Any&) = delete;
	Any(Any&&) = default;
	Any& operator=(Any&&) = default;

	// 这个构造函数可以让Any接受任意其他的数据
	template<typename T>
	Any(T data) :base_(std::make_unique<Derive<T>>(data)) {} 

	// 这个方法能把Any对象里面存储的data数据提取出来
	template <typename T>
	T cast_() {
		// 怎么从base_里面找到他所指向的Derive对象，从他里面取出data成员变量
		// 基类指针转为派生类指针 RTTI
		Derive<T>* pd = dynamic_cast<Derive<T>*>(base_.get());
		if (pd == nullptr) {
			throw "type is unmatch!";
		}
		return pd->data_;
	}
private:
	// 基类类型
	class Base {
	public:
		virtual ~Base() = default;
	};

	// 派生类类型
	template<typename T>
	class Derive :public Base {
	public:
		Derive(T data) :data_(data) {}
		T data_;   // 保存了任意的其他类型
	};

private:
	// 定义一个基类的指针
	std::unique_ptr<Base> base_;
};

// 实现一个信号量类
class Semaphore {
public:
	Semaphore(int limit = 0) :resLimit_(limit) {}
	
	~Semaphore() = default;

	// 获取一个信号量资源
	void wait() {
		std::unique_lock<std::mutex> lock(mtx_);
		// 等待信号量有资源
		cond_.wait(lock, [&]() {return resLimit_ > 0; });
		resLimit_--;
	}
	// 增加一个资源计数
	void post() {
		std::unique_lock<std::mutex> lock(mtx_);
		resLimit_++;
		// linux的condition_variable什么也没有做
		// 导致这里的状态已经失效，无故阻塞
		cond_.notify_all();   // 通知条件变量wait的地方可以工作了
	}

private:
	int resLimit_;
	std::mutex mtx_;
	std::condition_variable cond_;
};

// Task类型的前置声明
class Task;
// 实现提交到线程池的task任务执行完成后的返回值类型 Result
class Result {
public:
	Result(std::shared_ptr<Task> task, bool isvalid = true);
	~Result() = default;

	// 问题1：setVal方法，获取任务执行完的返回值的
	void setVal(Any any);
	// 问题2：get方法，用户调用这个方法获取task的返回值
	Any get();
private:
	Any any_;  // 存储任务的返回值
	Semaphore sem_;    // 线程通信信号量
	std::shared_ptr<Task> task_;  // 执行对应获取返回值的任务对象
	std::atomic_bool isValid_;
};

// 任务抽象基类
class Task {
public:
	Task();
	~Task() = default;
	void exec();
	void setResult(Result* res);
	// 用户可以自定义任意任务类型，从Task继承，重写run方法，实现自定义任务处理
	virtual Any run() = 0;
private:
	Result* result_;   // Result对象的声明周期是要强于Task的
};


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
	Thread(ThreadFunc func);
	// 线程析构
	~Thread();
	// 启动线程
	void start();

	// 获取线程id
	int getId() const;
private:
	ThreadFunc func_;   // 函数对象
	static int generateId_;
	int threadId_;	    // 保存线程id
};

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
	ThreadPool();

	// 线程池析构
	~ThreadPool();

	// 设置线程池的工作模式
	void setMode(PoolMode mode);

	// 设置task任务队列上限的阈值
	void setTaskQueMaxThreadHold(int threadhold);

	// 设置线程池cached模式下线程的阈值
	void setThreadSizeThreadHold(int threadhold);

	// 给线程池提交任务
	Result submitTask(std::shared_ptr<Task> sp);

	// 开启线程池   // 当前系统cpu的核心数量
	void start(int initThreadSize = std::thread::hardware_concurrency());

	// 禁止拷贝构造和赋值构造
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

private:
	// 定义线程函数
	void threadFunc(int threadid);

	// 检查pool的运行状态
	bool checkRunningState() const;

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
	std::queue<std::shared_ptr<Task>> taskQue_;        // 任务队列
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

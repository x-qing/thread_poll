#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <vector>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>    // ��������
#include <functional>
#include <unordered_map>
#include <thread>
#include <future>
#include <iostream>


const int TASK_MAX_THREADHOLD = 2; //  INT32_MAX;
const int THREAD_MAX_THREADHOLD = 1024;
const int THREAD_MAX_IDLE_TIME = 60;   //��λ��s

// ����class����ö�����ֲ�һ�����������������һ����
// �̳߳�֧�ֵ�����ģʽ
enum class PoolMode {
	MODE_FIXED,     // �̶��������߳�
	MODE_CACHED	// �߳������ɶ�̬����
};


// �߳�����
class Thread {
public:
	// �̺߳�����������
	using ThreadFunc = std::function<void(int)>;

	// �̹߳���
	Thread(ThreadFunc func) :func_(func), threadId_(generateId_++) {}
	// �߳�����
	~Thread() = default;
	// �����߳�
	void start() {
		// ����һ���߳���ִ��һ���̺߳���
		std::thread t(func_, threadId_);			// �̶߳���t���̺߳���func_
		t.detach();		 // ���÷��뺯��
	}

	// ��ȡ�߳�id
	int getId() const {
		return threadId_;
	}
private:
	ThreadFunc func_;   // ��������
	static int generateId_;
	int threadId_;	    // �����߳�id
};


int Thread::generateId_ = 0;

/*
* example:
* ThreadPool pool;
* pool.start(4);
* class Mytask:public Task{
*	public:
*		void run() {�̴߳���};
* }
* pool.submitTask(std::make_shared<MyTask>());
*
*/
// �̳߳�����
class ThreadPool {
public:

	// �̳߳ع���
	// ���캯��
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

	// �̳߳�����
	~ThreadPool() {
		isPoolRunning_ = false;
		//notEmpty_.notify_all();
		// �ȴ��̳߳��������е��̷߳���  ������״̬������ & ����ִ��������
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		notEmpty_.notify_all();
		/* Pool����������һֱ�����������Ϊ��ʱ��size����Ϊ0����Ϊ�̶߳������滹��һ���߳�*/
		exitCond_.wait(lock, [&]() { return threads_.size() == 0; });
	}

	// �����̳߳صĹ���ģʽ
	void setMode(PoolMode mode) {
		if (checkRunningState()) {
			return;
		}
		poolMode_ = mode;
	}

	// ����task����������޵���ֵ
	void setTaskQueMaxThreadHold(int threadhold) {
		if (checkRunningState()) {
			return;
		}
		taskQueMaxThreadHold_ = threadhold;
	}

	// �����̳߳�cachedģʽ���̵߳���ֵ
	void setThreadSizeThreadHold(int threadhold) {
		if (checkRunningState())
			return;
		if (poolMode_ == PoolMode::MODE_CACHED) {
			threadSizeThreadHold_ = threadhold;
		}
	}

	// ���̳߳��ύ����
	// ʹ�ÿɱ��ģ���̣���submitTask���Խ������⺯�������������Ĳ���
	template <typename Func,typename... Args>
	auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))> {
		// ������񣬷����������
		using RType = decltype(func(args...));   // �Ƶ���������һ������
		auto task = std::make_shared<std::packaged_task<RType()>>(
			std::bind(std::forward<Func>(func), std::forward<Args>(args)...)
			);
		std::future<RType> result = task->get_future();

		// ��ȡ��
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		// �̵߳�ͨ�� �ȴ���������п���
		if (!notFull_.wait_for(lock, std::chrono::seconds(1),
			[&]()->bool {return taskQue_.size() < (size_t)taskQueMaxThreadHold_; })) {
			// ��ʾnotFull���������ȴ�1s��������Ȼû������
			std::cerr << " task queue id full,submit task fail." << std::endl;
			auto task = std::make_shared<std::packaged_task<RType()>>(
				[]()->RType {return RType(); });
			(* task)();
			return task->get_future();
		}

		// ����п��࣬������������������
		//taskQue_.emplace(sp);
		taskQue_.emplace([task]() {
			// ȥִ���������
			(*task)();
			});
		taskSize_++;
		// ��Ϊ�·�������������п϶������ˣ���notEmpyt_ ֪ͨ�߳�ִ������
		notEmpty_.notify_all();

		// cachedģʽ ��Ҫ�������������Ϳ����̵߳��������ж��Ƿ���Ҫ�����µ��̣߳�
		// ����ȽϽ��������񣬳�����С���������
		if (poolMode_ == PoolMode::MODE_CACHED && taskSize_ > idleThreadSize_ &&
			curThreadSize_ < threadSizeThreadHold_) {
			std::cout << ">>>>>>>>> create new thread..." << std::endl;
			// �����µ��߳�
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			int threadId = ptr->getId();
			threads_.emplace(threadId, std::move(ptr));
			// �����߳�
			threads_[threadId]->start();
			// �޸��̸߳�����صı���
			curThreadSize_++;
			idleThreadSize_++;   // ���������߳�Ϊ�����߳�
			//threads_.emplace_back(std::move(ptr));
		}

		// ���������Result����
		return result;
	}

	// �����̳߳�   // ��ǰϵͳcpu�ĺ�������
	void start(int initThreadSize = std::thread::hardware_concurrency()) {
		// �����̳߳ص�����״̬
		isPoolRunning_ = true;
		// ��¼��ʼ�̵߳ĸ���
		initThreadSize_ = initThreadSize;
		curThreadSize_ = initThreadSize;

		// �����̶߳���
		for (int i = 0; i < initThreadSize_; i++) {
			// �󶨣�����Thread�����start�����Ϳ��Ե��� ThreadPool��threadFunc������
			//threads_.emplace_back(new Thread(std::bind(&ThreadPool::threadFunc,this)));
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			int threadId = ptr->getId();
			threads_.emplace(threadId, std::move(ptr));
			//threads_.emplace_back(std::move(ptr));
		}

		// ���������߳�
		for (int i = 0; i < initThreadSize_; i++) {
			threads_[i]->start();   // ��Ҫִ��һ���̺߳���
			idleThreadSize_++;   // ��¼��ʼ�����̵߳�����
		}
	}

	// ��ֹ��������͸�ֵ����
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

private:
	// �����̺߳���
	void threadFunc(int threadid) {
		auto lastTime = std::chrono::high_resolution_clock().now();


		// �����������ִ����ɣ��̳߳زſ��Ի������е��߳���Դ
		// while (isPoolRunning_)
		for (;;) {
			//std::shared_ptr<Task> task;
			Task task;
			{
				// ��ȡ��
				std::unique_lock<std::mutex> lock(taskQueMtx_);

				std::cout << "tid: " << std::this_thread::get_id() << "���Ի�ȡ����..." << std::endl;

				// cachedģʽ�£��п����Ѿ������˺ܶ��̣߳����ǿ����¼�����60s��Ӧ�ðѶ�����̻߳��յ�
				// ����initThreadsize_���߳�Ҫ���л���
				// ��ǰʱ�� - ��һ���߳�ִ�е�ʱ�� > 60
				// ÿһ���ӷ���һ��
				// ��ô�����ǳ�ʱ���ػ��������񷵻�

				// ��+˫���ж�
				while (isPoolRunning_ && taskQue_.size() == 0) {
					if (poolMode_ == PoolMode::MODE_CACHED) {
						// ����������ʱ����
						if (std::cv_status::timeout == notEmpty_.wait_for(lock, std::chrono::seconds(1))) {
							auto now = std::chrono::high_resolution_clock().now();
							auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
							if (dur.count() >= 60 && curThreadSize_ > initThreadSize_) {
								// ���յ�ǰ�߳�
								// ��¼�߳���������ر�����ֵ
								// ���̶߳�����߳��б�������ɾ��
								// û�а취threadFunc��Thread����
								// threadid -> thread����->ɾ��
								threads_.erase(threadid);   // ��Ҫʹ��std::this_thread::getid()
								curThreadSize_--;
								idleThreadSize_--;
								std::cout << "threadid:" << std::this_thread::get_id() << "exit!" << std::endl;
								return;
							}
						}
					}
					else {
						// �ȴ�notEmpty����
						notEmpty_.wait(lock);
					}
				}
				if (!isPoolRunning_) {
					break;
				}
				idleThreadSize_--;

				std::cout << "tid: " << std::this_thread::get_id() << "��ȡ����ɹ�..." << std::endl;
				// ȡһ������
				task = taskQue_.front();
				taskQue_.pop();
				taskSize_--;

				// �����Ȼ��ʣ�����񣬼���֪ͨ�������߳�ִ������
				if (taskQue_.size() > 0) {
					notEmpty_.notify_all();
				}

				// ȡ��һ���������֪ͨ,֪ͨ���Լ����ύ��������

				notFull_.notify_all();
			}// �ͷ���

			// ��ǰ�̸߳���ִ���������
			if (task != nullptr) {
				task();   // ִ��function<void()>������
			}
			idleThreadSize_++;
			// �����߳�ִ���������ʱ��
			auto lastTime = std::chrono::high_resolution_clock().now();
		}
		// �����̵߳������
		// һ�֣�ԭ�����߳̾�������
		// ��һ�֣��߳�����ִ������
		// 
		threads_.erase(threadid);   // ��Ҫʹ��std::this_thread::getid()
		std::cout << "threadid:" << std::this_thread::get_id() << "exit!" << std::endl;
		exitCond_.notify_all();
	}

	// ���pool������״̬
	bool checkRunningState() const {
		return isPoolRunning_;
	}

private:
	//std::vector<std::unique_ptr<Thread>> threads_;   // �߳��б�
	std::unordered_map<int, std::unique_ptr<Thread>> threads_;
	int initThreadSize_;			 // ��ʼ���߳�����
	int threadSizeThreadHold_;        // �߳��������޵���ֵ
	std::atomic_int curThreadSize_;  // ��¼��ǰ�̳߳������̵߳�������
	std::atomic_int idleThreadSize_;	// ��¼�����̵߳�����

	// ֱ����Task* ������ɾֲ�ָ�������,�û������Task���������ڿ��ܷǳ��Ķ�
	// ʹ������ָ��������������������ڣ�ͬʱ�Զ���������
	//std::queue<Task*> 

	// Task���� == ��������
	using Task = std::function<void()>;
	std::queue<Task> taskQue_;        // �������
	std::atomic_int taskSize_;        // ���������
	int taskQueMaxThreadHold_;        // ����������޵���ֵ

	std::mutex taskQueMtx_;       // ��֤������е��̰߳�ȫ
	std::condition_variable notFull_;        // ��ʾ������в���
	std::condition_variable notEmpty_;        // ��ʾ������в���

	std::condition_variable exitCond_;    // �ȴ��߳���Դȫ������

	PoolMode poolMode_;       // ��ǰ�̳߳صĹ���ģʽ

	// ��ʾ��ǰ�̳߳ص�����״̬
	std::atomic_bool isPoolRunning_;

};


// 


#endif // !THREADPOOL

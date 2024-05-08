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

// Any���ͣ����Խ����������ݵ�����
class Any {
public:
	Any() = default;
	~Any() = default;
	Any(const Any&) = delete;
	Any& operator=(const Any&) = delete;
	Any(Any&&) = default;
	Any& operator=(Any&&) = default;

	// ������캯��������Any������������������
	template<typename T>
	Any(T data) :base_(std::make_unique<Derive<T>>(data)) {} 

	// ��������ܰ�Any��������洢��data������ȡ����
	template <typename T>
	T cast_() {
		// ��ô��base_�����ҵ�����ָ���Derive���󣬴�������ȡ��data��Ա����
		// ����ָ��תΪ������ָ�� RTTI
		Derive<T>* pd = dynamic_cast<Derive<T>*>(base_.get());
		if (pd == nullptr) {
			throw "type is unmatch!";
		}
		return pd->data_;
	}
private:
	// ��������
	class Base {
	public:
		virtual ~Base() = default;
	};

	// ����������
	template<typename T>
	class Derive :public Base {
	public:
		Derive(T data) :data_(data) {}
		T data_;   // �������������������
	};

private:
	// ����һ�������ָ��
	std::unique_ptr<Base> base_;
};

// ʵ��һ���ź�����
class Semaphore {
public:
	Semaphore(int limit = 0) :resLimit_(limit) {}
	
	~Semaphore() = default;

	// ��ȡһ���ź�����Դ
	void wait() {
		std::unique_lock<std::mutex> lock(mtx_);
		// �ȴ��ź�������Դ
		cond_.wait(lock, [&]() {return resLimit_ > 0; });
		resLimit_--;
	}
	// ����һ����Դ����
	void post() {
		std::unique_lock<std::mutex> lock(mtx_);
		resLimit_++;
		// linux��condition_variableʲôҲû����
		// ���������״̬�Ѿ�ʧЧ���޹�����
		cond_.notify_all();   // ֪ͨ��������wait�ĵط����Թ�����
	}

private:
	int resLimit_;
	std::mutex mtx_;
	std::condition_variable cond_;
};

// Task���͵�ǰ������
class Task;
// ʵ���ύ���̳߳ص�task����ִ����ɺ�ķ���ֵ���� Result
class Result {
public:
	Result(std::shared_ptr<Task> task, bool isvalid = true);
	~Result() = default;

	// ����1��setVal��������ȡ����ִ����ķ���ֵ��
	void setVal(Any any);
	// ����2��get�������û��������������ȡtask�ķ���ֵ
	Any get();
private:
	Any any_;  // �洢����ķ���ֵ
	Semaphore sem_;    // �߳�ͨ���ź���
	std::shared_ptr<Task> task_;  // ִ�ж�Ӧ��ȡ����ֵ���������
	std::atomic_bool isValid_;
};

// ����������
class Task {
public:
	Task();
	~Task() = default;
	void exec();
	void setResult(Result* res);
	// �û������Զ��������������ͣ���Task�̳У���дrun������ʵ���Զ���������
	virtual Any run() = 0;
private:
	Result* result_;   // Result���������������Ҫǿ��Task��
};


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
	Thread(ThreadFunc func);
	// �߳�����
	~Thread();
	// �����߳�
	void start();

	// ��ȡ�߳�id
	int getId() const;
private:
	ThreadFunc func_;   // ��������
	static int generateId_;
	int threadId_;	    // �����߳�id
};

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
	ThreadPool();

	// �̳߳�����
	~ThreadPool();

	// �����̳߳صĹ���ģʽ
	void setMode(PoolMode mode);

	// ����task����������޵���ֵ
	void setTaskQueMaxThreadHold(int threadhold);

	// �����̳߳�cachedģʽ���̵߳���ֵ
	void setThreadSizeThreadHold(int threadhold);

	// ���̳߳��ύ����
	Result submitTask(std::shared_ptr<Task> sp);

	// �����̳߳�   // ��ǰϵͳcpu�ĺ�������
	void start(int initThreadSize = std::thread::hardware_concurrency());

	// ��ֹ��������͸�ֵ����
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

private:
	// �����̺߳���
	void threadFunc(int threadid);

	// ���pool������״̬
	bool checkRunningState() const;

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
	std::queue<std::shared_ptr<Task>> taskQue_;        // �������
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

#include "threadpool.h"
#include <functional>
#include <thread>
#include <iostream>
 
const int TASK_MAX_THREADHOLD = INT32_MAX;
const int THREAD_MAX_THREADHOLD = 1024;
const int THREAD_MAX_IDLE_TIME = 60;   //��λ��s

// ���캯��
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


// ��������
ThreadPool::~ThreadPool() {
	isPoolRunning_ = false;
	//notEmpty_.notify_all();
	// �ȴ��̳߳��������е��̷߳���  ������״̬������ & ����ִ��������
	std::unique_lock<std::mutex> lock(taskQueMtx_);
	notEmpty_.notify_all();
	/* Pool����������һֱ�����������Ϊ��ʱ��size����Ϊ0����Ϊ�̶߳������滹��һ���߳�*/
	exitCond_.wait(lock, [&]() { return threads_.size() == 0; });
}


// �����̳߳صĹ���ģʽ
void ThreadPool::setMode(PoolMode mode) {
	if (checkRunningState()) {
		return;
	}
	poolMode_ = mode;
}

// ����task����������޵���ֵ
void ThreadPool::setTaskQueMaxThreadHold(int threadhold) {
	if (checkRunningState()) {
		return;
	}
	taskQueMaxThreadHold_ = threadhold;
}

// ���̳߳��ύ����
// �û����øýӿڣ��������������������
Result ThreadPool::submitTask(std::shared_ptr<Task> sp) {
	// ��ȡ��
	std::unique_lock<std::mutex> lock(taskQueMtx_);
	// �̵߳�ͨ�� �ȴ���������п���
	//while (taskQue_.size() == taskQueMaxThreadHold_) {
	//	notFull_.wait(lock);  // ����ȴ�״̬
	//}
	if (!notFull_.wait_for(lock, std::chrono::seconds(1),
		[&]()->bool {return taskQue_.size() < (size_t)taskQueMaxThreadHold_; })) {
		// ��ʾnotFull���������ȴ�1s��������Ȼû������
		std::cerr << " task queue id full,submit task fail." << std::endl;
		//return task->getResult(); �в�ͨ���߳�ִ����task��task����ͱ�������
		return Result(sp,false); // Task Reslut
	}

	// ����п��࣬������������������
	taskQue_.emplace(sp);
	taskSize_++;
	// ��Ϊ�·�������������п϶������ˣ���notEmpyt_ ֪ͨ�߳�ִ������
	notEmpty_.notify_all();

	// cachedģʽ ��Ҫ�������������Ϳ����̵߳��������ж��Ƿ���Ҫ�����µ��̣߳�
	// ����ȽϽ��������񣬳�����С���������
	if (poolMode_ == PoolMode::MODE_CACHED && taskSize_ > idleThreadSize_ &&
		curThreadSize_ < threadSizeThreadHold_) {
		std::cout << ">>>>>>>>> create new thread..." << std::endl;
		// �����µ��߳�
		auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this,std::placeholders::_1));
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
	return Result(sp);
}

// �����̳߳�
void ThreadPool::start(int initThreadSize) {
	
	// �����̳߳ص�����״̬
	isPoolRunning_ = true;
	// ��¼��ʼ�̵߳ĸ���
	initThreadSize_ = initThreadSize;
	curThreadSize_ = initThreadSize;

	// �����̶߳���
	for (int i = 0; i < initThreadSize_; i++) {
		// �󶨣�����Thread�����start�����Ϳ��Ե��� ThreadPool��threadFunc������
		//threads_.emplace_back(new Thread(std::bind(&ThreadPool::threadFunc,this)));
		auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this,std::placeholders::_1));
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

// �������������̳߳��еĺ����ͷǳ��ķ�����
// �̳߳ص������̴߳��������������������
void ThreadPool::threadFunc(int threadid) {
	//std::cout << "begin threadFunc tid: " << std::this_thread::get_id() <<  std::endl;
	//std::cout << "end threadFunc tid :" << std::this_thread::get_id() << std::endl;

	auto lastTime = std::chrono::high_resolution_clock().now();


	// �����������ִ����ɣ��̳߳زſ��Ի������е��߳���Դ
	// while (isPoolRunning_)
	for (;;)  {
		std::shared_ptr<Task> task;
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
			while (isPoolRunning_ && taskQue_.size() == 0){
				if(poolMode_ == PoolMode::MODE_CACHED){
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

				// �ȴ����̱߳����Ѻ󣬱�Ϊ��������ȡ��֮������������isPoolRunning_Ϊfalse�ˡ���ô���߳���ɾ��
				// �̳߳�Ҫ�����������߳���Դ
				//if (!isPoolRunning_) {
				//	threads_.erase(threadid);   // ��Ҫʹ��std::this_thread::getid()
				//	std::cout << "threadid:" << std::this_thread::get_id() << "exit!" << std::endl;
				//	exitCond_.notify_all();
				//	return;
				//}
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
			//task->run();   // ִ�����񣬰�����ķ���ֵsetVal��������Result
			task->exec();
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


/////////////////////  �̷߳�ʽʵ��  /////////////////////

int Thread::generateId_ = 0;

// �̹߳���
Thread::Thread(ThreadFunc func) :func_(func),threadId_(generateId_++) {}
// �߳�����
Thread::~Thread() {}

// �����߳�
void Thread::start() {
	// ����һ���߳���ִ��һ���̺߳���
	std::thread t(func_,threadId_);			// �̶߳���t���̺߳���func_
	t.detach();		 // ���÷��뺯��
}

int Thread::getId() const {
	return threadId_;
}

////////////////////////// Task����ʵ��   //////////////////////////////
Task::Task():result_(nullptr){}

void Task::exec() {

	if (result_ != nullptr) {
		result_->setVal(run());   // ���﷢����̬����
	}
}

void Task::setResult(Result* res) {
	result_ = res;
}

///////////////////////////Result������ʵ��/////////////////////
Result::Result(std::shared_ptr<Task> task, bool isvalid):isValid_(isvalid),task_(task) {
	task_->setResult(this);
}


// �û����õ�
Any Result::get() {
	if (!isValid_) {
		return "";
	}
	sem_.wait();   // task����û��ִ���꣬���������û����߳�
	return std::move(any_);
}

// 
void Result::setVal(Any any) {
	// �洢task�ķ���ֵ
	this->any_ = std::move(any);
	sem_.post();   // �Ѿ���ȡ������ķ���ֵ�������ź�����Դ

}
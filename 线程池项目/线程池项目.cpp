// 线程池项目.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//

#include <iostream>
#include "threadpool.h"

#include <chrono>
#include <thread>

using uLong = unsigned long long;

class MyTask : public Task {
public:
    MyTask(uLong begin, uLong end) :begin_(begin), end_(end) {};

    // 问题1：怎么设计run函数的返回值，可以表示任意的类型
    // java python  object 是所有其他类类型的基类
    // c++17 也提供了一个，Any类型
    Any run() {   // run方法最终在线程池分配
		std::cout << "tid: " << std::this_thread::get_id() << "begin!" << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(3));
        uLong sum = 0;
        for (uLong i = begin_; i <= end_; i++) {
            sum += i;
        }
        std::cout << "tid :" << std::this_thread::get_id() << "end!" << std::endl;
        return sum;
    }
private:
    uLong begin_;
    uLong end_;
};



int main()
{
    {
		ThreadPool pool;
        pool.setMode(PoolMode::MODE_CACHED);
		pool.start(2);

		Result res1 = pool.submitTask(std::make_shared<MyTask>(1, 100000000));
        pool.submitTask(std::make_shared<MyTask>(1, 100000000));
        pool.submitTask(std::make_shared<MyTask>(1, 100000000));
        pool.submitTask(std::make_shared<MyTask>(1, 100000000));
        pool.submitTask(std::make_shared<MyTask>(1, 100000000));

		uLong sum1 = res1.get().cast_<uLong>();
		std::cout << sum1 << std::endl;
    }
    // !!!这里的result对象也要析构
    // 在vs下，条件变量析构会释放相应资源

    std::cout << "main over!" << std::endl;

    getchar();
#if 0
    // 问题：threadPool对象析构以后，怎么样把线程相关的线程资源全部回收？
    {
		ThreadPool pool;

		// 用户自己设置线程池的工作模式
		//pool.setMode();
		pool.setMode(PoolMode::MODE_CACHED);
		// 开始启动工作池
		pool.start();

		// 问题2：如何设计result机制
		Result res1 = pool.submitTask(std::make_shared<MyTask>(1, 100000000));
		Result res2 = pool.submitTask(std::make_shared<MyTask>(100000001, 200000000));
		Result res3 = pool.submitTask(std::make_shared<MyTask>(200000001, 300000000));

		Result res4 = pool.submitTask(std::make_shared<MyTask>(200000001, 300000000));
		Result res5 = pool.submitTask(std::make_shared<MyTask>(200000001, 300000000));
		Result res6 = pool.submitTask(std::make_shared<MyTask>(200000001, 300000000));



		// 随着task被执行完，task对象没有了，依赖于task对象的Result对象也没有了
		//int sum = res.get().cast_<>();
		//res.get();   // 返回了一个Any类型，怎么转成具体的类型
		uLong sum1 = res1.get().cast_<uLong>();
		uLong sum2 = res2.get().cast_<uLong>();
		uLong sum3 = res3.get().cast_<uLong>();

		// Master - Slave线程模型，
		// Master线程用来分解任务，然后给各个Salve线程分配任务
		std::cout << (sum1 + sum2 + sum3) << std::endl;

		uLong sum = 0;
		for (uLong i = 1; i <= 300000000; i++) {
			sum += i;
		}
		std::cout << sum << std::endl;
    }


    //pool.submitTask(std::make_shared<MyTask>());
    //pool.submitTask(std::make_shared<MyTask>());
    //pool.submitTask(std::make_shared<MyTask>());
    //pool.submitTask(std::make_shared<MyTask>());
    //pool.submitTask(std::make_shared<MyTask>());
    //pool.submitTask(std::make_shared<MyTask>());
    //pool.submitTask(std::make_shared<MyTask>());
    //pool.submitTask(std::make_shared<MyTask>());
    //pool.submitTask(std::make_shared<MyTask>());

    getchar();
    //std::this_thread::sleep_for(std::chrono::seconds(5));

    return 0;
    //std::cout << "Hello World!\n";


#endif
}
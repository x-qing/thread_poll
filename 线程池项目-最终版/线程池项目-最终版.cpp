#include <iostream>
#include <thread>
#include <functional>
#include <future> 
#include <chrono>
#include "threadpool.h"
using namespace std;

/*
1. 如何让线程池提交任务更加方便
    pool.submitTask(sum1,10,20);
    pool.submitTask(sum2,1,2,3);

    submitTask可变参模板编程
2. 自己造了一个Result以及相关的类型
    C++11线程库 thread   packaged_task(function函数对象)

    使用future来代替Result节省线程池代码

*/

int sum1(int a, int b) {
    std::this_thread::sleep_for(std::chrono::seconds(3));
    return a + b;
}

int sum2(int a, int b, int c) {
    std::this_thread::sleep_for(std::chrono::seconds(3));
    return a + b + c;
}

int main()
{
    ThreadPool pool;
    //pool.setMode(PoolMode::MODE_CACHED);
    pool.start(2);
    future<int> r1 = pool.submitTask(sum1, 1, 2);

    future<int> r2 = pool.submitTask(sum2, 10, 20,30);
    future<int> r3 = pool.submitTask([](int b, int e)->int {
        int sum = 0;
        for (int i = b; i <= e; i++) {
            sum += i;
        }
        return sum;
        }, 1, 100);
    future<int> r4 = pool.submitTask([](int b,int e)->int{
        int sum = 0;
        for (int i = b; i <= e; i++) {
            sum += i;
        }
        return sum;
    },1,100);

    future<int> r5 = pool.submitTask([](int b, int e)->int {
        int sum = 0;
        for (int i = b; i <= e; i++) {
            sum += i;
        }
        return sum;
        }, 1, 100);

    cout << r1.get() << endl;
    cout << r2.get() << endl;
    cout << r3.get() << endl;
    cout << r4.get() << endl;
    cout << r5.get() << endl;

    //getchar();
    /*
    packaged_task<int(int, int)> task{ sum1 };   // 打包了一个任务，支持一个方法，可以获取返回值
    // future == result
    future<int> res = task.get_future();
    //task(10, 20);
    thread t(std::move(task), 10, 20);
    t.detach();

    cout << res.get() << endl;


    //thread t1(sum1, 10, 20);
    //thread t2(sum2, 1, 2, 3);

    //t1.join();
    //t2.join();
    //std::cout << "Hello World!\n";

    */
}

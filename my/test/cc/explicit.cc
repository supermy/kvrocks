#include <iostream>
using namespace std;

// vector <int> ivec(10);  //这种定义看起来一目了然
// 不能写成vector <int> ivec=10；//此种定义让程序员感到疑惑
// 何时不用explicit，当我们需要隐式转换的时候，比如说String类的一个构造函数
// String(const char*);

class A
{
public:
    explicit A(int a)
    {
        cout << "创建类成功了!" << endl;
    }
};
int main()
{
    // A a = 10; //编译失败 test/cc/explicit.cc:14:7: error: no viable conversion from 'int' to 'A'
    A a(10);

    // std::String str="helloworld";
    // std::String str="hello"+str+"world";


    return 0;
}

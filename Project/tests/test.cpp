#include <iostream>
class Test {
public:
    int a;
    Test() {
        a = 10;
        std::cout << "Constructor is executed\n";
    }
    ~Test() {
        std::cout << "Destructor is executed\n";
    }
};
static Test obj;
int main() {
    std::cout << "main() starts\n";
    std::cout << obj.a; //注意：静态对象可以调用它的所有成员，包括非静态成员。
                        //但如果是静态函数，则只能调用静态成员。
    std::cout << "\nmain() terminates\n";
    return 0;
}
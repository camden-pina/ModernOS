#include <iostream>
#include <stdexcept>
#include <thread>
int main() {
    int value = 0;
    std::thread worker([&] {
        try {
            throw std::runtime_error("native");
        } catch (const std::exception &) {
            value = 42;
        }
    });
    worker.join();
    if (value != 42)
        return 1;
    std::cout << "DYNAMIC CPP NATIVE COMPILE PASS\n";
}

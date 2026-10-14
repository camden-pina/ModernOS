#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int main() {
    const std::vector<std::string> words = {"NATIVE", "CPP"};
    try {
        throw std::runtime_error(words[0] + "_" + words[1] + "_OK");
    } catch (const std::exception &error) {
        std::cout << error.what() << std::endl;
    }
}

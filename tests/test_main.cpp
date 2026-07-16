#include <gtest/gtest.h>
#include "logger.hpp"

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    Logger::instance().set_level(LogLevel::FATAL);
    return RUN_ALL_TESTS();
}

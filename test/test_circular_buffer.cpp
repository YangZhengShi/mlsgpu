/**
 * @file
 *
 * Test code for @ref CircularBuffer.
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <cstddef>
#include <algorithm>
#include <cppunit/extensions/TestFactoryRegistry.h>
#include <cppunit/extensions/HelperMacros.h>
#include <boost/bind.hpp>
#include <boost/tr1/random.hpp>
#include <boost/thread.hpp>
#include "testmain.h"
#include "../src/circular_buffer.h"
#include "../src/work_queue.h"
#include "../src/tr1_cstdint.h"
#include "../src/statistics.h"

/**
 * Functionality tests for @ref CircularBuffer. These tests do not exercise
 * any blocking-related behavior, as that is covered in @ref
 * TestCircularBufferStress.
 */
class TestCircularBuffer : public CppUnit::TestFixture
{
    CPPUNIT_TEST_SUITE(TestCircularBuffer);
    CPPUNIT_TEST(testAllocateFree);
    CPPUNIT_TEST(testSize);
    CPPUNIT_TEST(testStatistics);
    CPPUNIT_TEST(testBigMax);
    CPPUNIT_TEST(testElementTooLarge);
    CPPUNIT_TEST(testMaxZero);
    CPPUNIT_TEST_SUITE_END();

private:
    void testAllocateFree();    ///< Smoke test for @ref CircularBuffer::allocate and @ref CircularBuffer::free
    void testSize();            ///< Test @ref CircularBuffer::size
    void testStatistics();      ///< Test that memory allocation is accounted for
    void testBigMax();          ///< Test that no overflow occurs when @a maxElements is huge
    void testElementTooLarge(); ///< Test that an exception is thrown for a huge element size
    void testMaxZero();         ///< Test that an exception is thrown when asking for zero elements
};
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(TestCircularBuffer, TestSet::perBuild());

void TestCircularBuffer::testAllocateFree()
{
    CircularBuffer buffer("test", 10);
    std::pair<void *, std::size_t> item = buffer.allocate(sizeof(short), 2);
    CPPUNIT_ASSERT(item.first != NULL);
    CPPUNIT_ASSERT(item.second >= 1 && item.second <= 2);

    // Check that the memory can be safely written
    short *values = reinterpret_cast<short *>(item.first);
    values[0] = 123;
    values[1] = 456;

    buffer.free(item.first, sizeof(short), item.second);
}

void TestCircularBuffer::testSize()
{
    CircularBuffer buffer("test", 1000);
    CPPUNIT_ASSERT_EQUAL(std::size_t(1000), buffer.size());
}

void TestCircularBuffer::testStatistics()
{
    typedef Statistics::Peak<std::size_t> Peak;
    Peak &allStat = Statistics::getStatistic<Peak>("mem.all");
    std::size_t oldMem = allStat.get();

    CircularBuffer buffer("test", 1000);

    std::size_t newMem = allStat.get();
    CPPUNIT_ASSERT_EQUAL(oldMem + 1000, newMem);
}

void TestCircularBuffer::testBigMax()
{
    CircularBuffer buffer("test", 1000);
    std::pair<void *, std::size_t> item = buffer.allocate(4, 0x1000000000000);
    CPPUNIT_ASSERT(item.first != NULL);
    CPPUNIT_ASSERT(item.second > 0);
    CPPUNIT_ASSERT(item.second <= 1000);
}

void TestCircularBuffer::testElementTooLarge()
{
    CircularBuffer buffer("test", 16);
    CPPUNIT_ASSERT_THROW(buffer.allocate(12, 4), std::invalid_argument);
}

void TestCircularBuffer::testMaxZero()
{
    CircularBuffer buffer("test", 16);
    CPPUNIT_ASSERT_THROW(buffer.allocate(4, 0), std::invalid_argument);
}

/// Stress tests for @ref CircularBuffer
class TestCircularBufferStress : public CppUnit::TestFixture
{
    CPPUNIT_TEST_SUITE(TestCircularBufferStress);
    CPPUNIT_TEST(testStress);
    CPPUNIT_TEST_SUITE_END();

public:
    TestCircularBufferStress()
        : buffer("mem.TestCircularBufferStress", 123), workQueue(10) {}

private:
    struct Item
    {
        std::tr1::uint64_t *ptr;
        std::size_t elements;
    };

    CircularBuffer buffer;
    WorkQueue<Item> workQueue;   ///< Ranges sent from producer to consumer

    /**
     * Generates the numbers from 0 up to @a total and places them
     * in chunks of the buffer. The subranges are enqueued on @ref workQueue.
     */
    void producerThread(std::tr1::uint64_t total);

    /**
     * Pass a lot of numbers from @ref producerThread to the main thread,
     * checking that they arrive correctly formed.
     */
    void testStress();
};
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(TestCircularBufferStress, TestSet::perCommit());

void TestCircularBufferStress::producerThread(std::tr1::uint64_t total)
{
    std::tr1::mt19937 engine;
    std::tr1::uint64_t cur = 0;
    std::tr1::uniform_int<std::tr1::uint32_t> chunkDist(1, buffer.size() * 2 / sizeof(cur));

    while (cur < total)
    {
        std::tr1::uint64_t max = chunkDist(engine);
        max = std::min(max, total - cur);
        std::pair<void *, std::size_t> chunk = buffer.allocate(sizeof(cur), max);
        CPPUNIT_ASSERT(chunk.first != NULL);
        CPPUNIT_ASSERT(chunk.second > 0);
        CPPUNIT_ASSERT(chunk.second <= max);

        std::tr1::uint64_t *ptr = static_cast<std::tr1::uint64_t *>(chunk.first);
        for (std::size_t i = 0; i < chunk.second; i++)
        {
            ptr[i] = cur++;
        }

        Item item;
        item.ptr = ptr;
        item.elements = chunk.second;
        workQueue.push(item);
    }

    Item item;
    item.ptr = NULL;
    workQueue.push(item);
}

void TestCircularBufferStress::testStress()
{
    const std::size_t total = 10000000;
    boost::thread producer(boost::bind(&TestCircularBufferStress::producerThread, this, total));

    std::tr1::uint64_t expect = 0;
    Item item;

    /* This generator doesn't do anything useful - it's just a way to
     * make sure that the producer and consumer run at about the same
     * rate and hence test both full and empty conditions.
     */
    std::tr1::mt19937 gen;
    std::tr1::uniform_int<std::tr1::uint32_t> chunkDist(1, buffer.size() * 2 / sizeof(std::tr1::uint64_t));

    while ((item = workQueue.pop()).ptr != NULL)
    {
        CPPUNIT_ASSERT(item.elements > 0 && item.elements < buffer.size());
        for (std::size_t i = 0; i < item.elements; i++)
        {
            CPPUNIT_ASSERT_EQUAL(expect, item.ptr[i]);
            expect++;
        }
        buffer.free(item.ptr, sizeof(std::tr1::uint64_t), item.elements);
        (void) chunkDist(gen);
    }
    CPPUNIT_ASSERT_EQUAL(total, expect);

    producer.join();
}

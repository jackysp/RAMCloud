/* Copyright (c) 2009-2010 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <Common.h>

#include <HashTable.h>

#include <cppunit/extensions/HelperMacros.h>

namespace RAMCloud {

/**
 * Unit tests for HashTable::Entry.
 */
class HashTableEntryTest : public CppUnit::TestFixture {

    //HashTable::Entry entries[10];

    DISALLOW_COPY_AND_ASSIGN(HashTableEntryTest); // NOLINT

    CPPUNIT_TEST_SUITE(HashTableEntryTest);
    CPPUNIT_TEST(test_size);
    CPPUNIT_TEST(test_pack);
    CPPUNIT_TEST(test_clear);
    CPPUNIT_TEST(test_setLogPointer);
    CPPUNIT_TEST(test_setChainPointer);
    CPPUNIT_TEST(test_isAvailable);
    CPPUNIT_TEST(test_getLogPointer);
    CPPUNIT_TEST(test_getChainPointer);
    CPPUNIT_TEST(test_hashMatches);
    CPPUNIT_TEST(test_isChainLink);
    CPPUNIT_TEST_SUITE_END();

    static bool
    packable(uint64_t hash, bool chain, uint64_t ptr)
    {
        HashTable::Entry e;

        HashTable::Entry::UnpackedEntry in;
        HashTable::Entry::UnpackedEntry out;

        in.hash = hash;
        in.chain = chain;
        in.ptr = reinterpret_cast<void*>(ptr);

        e.pack(in.hash, in.chain, in.ptr);
        out = e.unpack();

        return (in.hash == out.hash &&
                in.chain == out.chain &&
                in.ptr == out.ptr);
    }

  public:
    HashTableEntryTest() {}

    void test_size()
    {
        CPPUNIT_ASSERT(8 == sizeof(HashTable::Entry));
    }

    void test_pack()
    {
        CPPUNIT_ASSERT(packable(0x0000UL, false, 0x000000000000UL));
        CPPUNIT_ASSERT(packable(0xffffUL, true,  0x7fffffffffffUL));
        CPPUNIT_ASSERT(packable(0xffffUL, false, 0x7fffffffffffUL));
        CPPUNIT_ASSERT(packable(0xa257UL, false, 0x3cdeadbeef98UL));
    }

    void test_clear()
    {
        HashTable::Entry e;
        e.value = 0xdeadbeefdeadbeefUL;
        e.clear();
        HashTable::Entry::UnpackedEntry out;
        out = e.unpack();
        CPPUNIT_ASSERT_EQUAL(0UL, out.hash);
        CPPUNIT_ASSERT_EQUAL(false, out.chain);
        CPPUNIT_ASSERT_EQUAL(reinterpret_cast<void*>(NULL), out.ptr);
    }

    void test_setLogPointer()
    {
        HashTable::Entry e;
        e.value = 0xdeadbeefdeadbeefUL;
        e.setLogPointer(0xaaaaUL, reinterpret_cast<void*>(0x7fffffffffffUL));
        HashTable::Entry::UnpackedEntry out;
        out = e.unpack();
        CPPUNIT_ASSERT_EQUAL(0xaaaaUL, out.hash);
        CPPUNIT_ASSERT_EQUAL(false, out.chain);
        CPPUNIT_ASSERT_EQUAL(reinterpret_cast<void*>(0x7fffffffffffUL),
                             out.ptr);
    }

    void test_setChainPointer()
    {
        HashTable::Entry e;
        e.value = 0xdeadbeefdeadbeefUL;
        e.setChainPointer((HashTable::CacheLine*) 0x7fffffffffffUL);
        HashTable::Entry::UnpackedEntry out;
        out = e.unpack();
        CPPUNIT_ASSERT_EQUAL(0UL, out.hash);
        CPPUNIT_ASSERT_EQUAL(true, out.chain);
        CPPUNIT_ASSERT_EQUAL(reinterpret_cast<void*>(0x7fffffffffffUL),
                             out.ptr);
    }

    void test_isAvailable()
    {
        HashTable::Entry e;
        e.clear();
        CPPUNIT_ASSERT(e.isAvailable());
        e.setChainPointer((HashTable::CacheLine*) 0x1UL);
        CPPUNIT_ASSERT(!e.isAvailable());
        e.setLogPointer(0UL, reinterpret_cast<void*>(0x1UL));
        CPPUNIT_ASSERT(!e.isAvailable());
        e.clear();
        CPPUNIT_ASSERT(e.isAvailable());
    }

    void test_getLogPointer()
    {
        HashTable::Entry e;
        e.setLogPointer(0xaaaaUL, reinterpret_cast<void*>(0x7fffffffffffUL));
        CPPUNIT_ASSERT_EQUAL(reinterpret_cast<void*>(0x7fffffffffffUL),
                             e.getLogPointer());
    }

    void test_getChainPointer()
    {
        HashTable::Entry e;
        e.setChainPointer((HashTable::CacheLine*) 0x7fffffffffffUL);
        CPPUNIT_ASSERT_EQUAL((HashTable::CacheLine*) 0x7fffffffffffUL,
                             e.getChainPointer());
    }

    void test_hashMatches()
    {
        HashTable::Entry e;
        e.clear();
        CPPUNIT_ASSERT(!e.hashMatches(0UL));
        e.setChainPointer((HashTable::CacheLine*) 0x1UL);
        CPPUNIT_ASSERT(!e.hashMatches(0UL));
        e.setLogPointer(0UL, reinterpret_cast<void*>(0x1UL));
        CPPUNIT_ASSERT(e.hashMatches(0UL));
        CPPUNIT_ASSERT(!e.hashMatches(0xbeefUL));
        e.setLogPointer(0xbeefUL, reinterpret_cast<void*>(0x1UL));
        CPPUNIT_ASSERT(!e.hashMatches(0UL));
        CPPUNIT_ASSERT(e.hashMatches(0xbeefUL));
        CPPUNIT_ASSERT(!e.hashMatches(0xfeedUL));
    }

    void test_isChainLink()
    {
        HashTable::Entry e;
        e.clear();
        CPPUNIT_ASSERT(!e.isChainLink());
        e.setChainPointer((HashTable::CacheLine*) 0x1UL);
        CPPUNIT_ASSERT(e.isChainLink());
        e.setLogPointer(0UL, reinterpret_cast<void*>(0x1UL));
        CPPUNIT_ASSERT(!e.isChainLink());
    }

};
CPPUNIT_TEST_SUITE_REGISTRATION(HashTableEntryTest);

/**
 * Unit tests for HashTable::Entry.
 */
class HashTablePerfDistributionTest : public CppUnit::TestFixture {

    //HashTable::Entry entries[10];

    DISALLOW_COPY_AND_ASSIGN(HashTablePerfDistributionTest); // NOLINT

    CPPUNIT_TEST_SUITE(HashTablePerfDistributionTest);
    CPPUNIT_TEST(test_constructor);
    CPPUNIT_TEST(test_storeSample);
    CPPUNIT_TEST_SUITE_END();

  public:
    HashTablePerfDistributionTest() {}

    void test_constructor()
    {
        RAMCloud::HashTable::PerfDistribution d;
        CPPUNIT_ASSERT_EQUAL(~0UL, d.min);
        CPPUNIT_ASSERT_EQUAL(0UL, d.max);
        CPPUNIT_ASSERT_EQUAL(0UL, d.binOverflows);
        CPPUNIT_ASSERT_EQUAL(0UL, d.bins[0]);
        CPPUNIT_ASSERT_EQUAL(0UL, d.bins[1]);
        CPPUNIT_ASSERT_EQUAL(0UL, d.bins[2]);
    }

    void test_storeSample()
    {
        HashTable::PerfDistribution d;

        // You can't use CPPUNIT_ASSERT_EQUAL here because it tries to take a
        // reference to BIN_WIDTH. See 10.4.6.2 Member Constants of The C++
        // Programming Language by Bjarne Stroustrup for more about static
        // constant integers.
        CPPUNIT_ASSERT(10 == HashTable::PerfDistribution::BIN_WIDTH);

        d.storeSample(3);
        CPPUNIT_ASSERT_EQUAL(3UL, d.min);
        CPPUNIT_ASSERT_EQUAL(3UL, d.max);
        CPPUNIT_ASSERT_EQUAL(0UL, d.binOverflows);
        CPPUNIT_ASSERT_EQUAL(1UL, d.bins[0]);
        CPPUNIT_ASSERT_EQUAL(0UL, d.bins[1]);
        CPPUNIT_ASSERT_EQUAL(0UL, d.bins[2]);

        d.storeSample(3);
        d.storeSample(d.NBINS * d.BIN_WIDTH + 40);
        d.storeSample(12);
        d.storeSample(78);

        CPPUNIT_ASSERT_EQUAL(3UL, d.min);
        CPPUNIT_ASSERT_EQUAL(d.NBINS * d.BIN_WIDTH + 40, d.max);
        CPPUNIT_ASSERT_EQUAL(1UL, d.binOverflows);
        CPPUNIT_ASSERT_EQUAL(2UL, d.bins[0]);
        CPPUNIT_ASSERT_EQUAL(1UL, d.bins[1]);
        CPPUNIT_ASSERT_EQUAL(0UL, d.bins[2]);
    }

};
CPPUNIT_TEST_SUITE_REGISTRATION(HashTablePerfDistributionTest);

class HashTableTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(HashTableTest);
    CPPUNIT_TEST(test_sizes);
    CPPUNIT_TEST(test_simple);
    CPPUNIT_TEST_SUITE_END();
    DISALLOW_COPY_AND_ASSIGN(HashTableTest); //NOLINT

  public:
    HashTableTest()
    {
    }

    void test_sizes()
    {
        // We're specifically aiming to fit in a cache line.
        CPPUNIT_ASSERT(8 == sizeof(HashTable::Entry));
        CPPUNIT_ASSERT(8 * HashTable::ENTRIES_PER_CACHE_LINE ==
                       sizeof(HashTable::CacheLine));
    }

    void test_simple()
    {
        RAMCloud::HashTable ht(1024);

        uint64_t a = 0;
        uint64_t b = 10;

        CPPUNIT_ASSERT(ht.lookup(0) == NULL);
        ht.insert(0, &a);
        CPPUNIT_ASSERT(ht.lookup(0) == &a);
        CPPUNIT_ASSERT(ht.lookup(10) == NULL);
        ht.insert(10, &b);
        CPPUNIT_ASSERT(ht.lookup(10) == &b);
        CPPUNIT_ASSERT(ht.lookup(0) == &a);
    }
};
CPPUNIT_TEST_SUITE_REGISTRATION(HashTableTest);

} // namespace RAMCloud

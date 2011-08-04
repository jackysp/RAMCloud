/* Copyright (c) 2010-2011 Stanford University
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

#include <boost/scoped_ptr.hpp>
#include "TestUtil.h"
#include "BackupManager.h"
#include "BackupService.h"
#include "BackupStorage.h"
#include "BindTransport.h"
#include "Buffer.h"
#include "ClientException.h"
#include "CoordinatorClient.h"
#include "CoordinatorService.h"
#include "Logging.h"
#include "MasterClient.h"
#include "MasterService.h"
#include "TransportManager.h"

namespace RAMCloud {

namespace {
struct ServerListBuilder {
    explicit ServerListBuilder(ProtoBuf::ServerList& servers)
        : servers(servers)
    {
    }

    ServerListBuilder&
    operator()(ProtoBuf::ServerType type,
               uint64_t id,
               uint64_t segmentId,
               const char* locator,
               uint64_t userData = 0)
    {
        ProtoBuf::ServerList_Entry& server(*servers.add_server());
        server.set_server_type(type);
        server.set_server_id(id);
        server.set_segment_id(segmentId);
        server.set_service_locator(locator);
        server.set_user_data(userData);
        return *this;
    }

    ProtoBuf::ServerList& servers;
};
}

class MasterServiceTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(MasterServiceTest);
    CPPUNIT_TEST(test_create_basics);
    CPPUNIT_TEST(test_create_badTable);
    CPPUNIT_TEST(test_ping);
    CPPUNIT_TEST(test_read_basics);
    CPPUNIT_TEST(test_read_badTable);
    CPPUNIT_TEST(test_read_noSuchObject);
    CPPUNIT_TEST(test_read_rejectRules);
    CPPUNIT_TEST(test_multiRead_basics);
    CPPUNIT_TEST(test_multiRead_badTable);
    CPPUNIT_TEST(test_multiRead_noSuchObject);
    CPPUNIT_TEST(test_detectSegmentRecoveryFailure_success);
    CPPUNIT_TEST(test_detectSegmentRecoveryFailure_failure);
    CPPUNIT_TEST(test_recover_basics);
    CPPUNIT_TEST(test_recover);
    CPPUNIT_TEST(test_recoverSegment);
    CPPUNIT_TEST(test_remove_basics);
    CPPUNIT_TEST(test_remove_badTable);
    CPPUNIT_TEST(test_remove_rejectRules);
    CPPUNIT_TEST(test_remove_objectAlreadyDeletedRejectRules);
    CPPUNIT_TEST(test_remove_objectAlreadyDeleted);
    CPPUNIT_TEST(test_setTablets);
    CPPUNIT_TEST(test_write);
    CPPUNIT_TEST(test_write_rejectRules);
    CPPUNIT_TEST(test_getTable);
    CPPUNIT_TEST(test_rejectOperation);
    CPPUNIT_TEST_SUITE_END();

  public:
    ServerConfig config;
    BackupService::Config backupConfig;
    BackupService* backupService;
    BackupStorage* storage;
    const uint32_t segmentFrames;
    const uint32_t segmentSize;
    MasterService* service;
    BindTransport* transport;
    MasterClient* client;
    CoordinatorClient* coordinator;
    CoordinatorService* coordinatorService;

    MasterServiceTest()
        : config()
        , backupConfig()
        , backupService()
        , storage(NULL)
        , segmentFrames(2)
        , segmentSize(1 << 16)
        , service(NULL)
        , transport(NULL)
        , client(NULL)
        , coordinator(NULL)
        , coordinatorService(NULL)
    {
        config.localLocator = "mock:host=master";
        config.coordinatorLocator = "mock:host=coordinator";
        backupConfig.coordinatorLocator = "mock:host=coordinator";
        MasterService::sizeLogAndHashTable("64", "8", &config);
    }

    void setUp() {
        logger.setLogLevels(SILENT_LOG_LEVEL);
        transport = new BindTransport();
        transportManager.registerMock(transport);
        coordinatorService = new CoordinatorService();
        transport->addService(*coordinatorService, "mock:host=coordinator");
        coordinator = new CoordinatorClient("mock:host=coordinator");

        storage = new InMemoryStorage(segmentSize, segmentFrames);
        backupService = new BackupService(backupConfig, *storage);
        transport->addService(*backupService, "mock:host=backup1");
        coordinator->enlistServer(BACKUP, "mock:host=backup1");

        service = new MasterService(config, coordinator, 1);
        transport->addService(*service, "mock:host=master");
        service->serverId.construct(
            coordinator->enlistServer(MASTER, config.localLocator));
        client =
            new MasterClient(transportManager.getSession("mock:host=master"));
        ProtoBuf::Tablets_Tablet& tablet(*service->tablets.add_tablet());
        tablet.set_table_id(0);
        tablet.set_start_object_id(0);
        tablet.set_end_object_id(~0UL);
        tablet.set_user_data(reinterpret_cast<uint64_t>(new Table(0)));
    }

    void tearDown() {
        delete client;
        delete service;
        delete backupService;
        delete storage;
        delete coordinator;
        delete coordinatorService;
        transportManager.unregisterMock();
        delete transport;
    }

    void test_create_basics() {
        uint64_t version;
        CPPUNIT_ASSERT_EQUAL(0, client->create(0, "item0", 5, &version));
        CPPUNIT_ASSERT_EQUAL(1, version);
        CPPUNIT_ASSERT_EQUAL(1, client->create(0, "item1", 5, &version));
        CPPUNIT_ASSERT_EQUAL(2, version);
        CPPUNIT_ASSERT_EQUAL(2, client->create(0, "item2", 5));

        Buffer value;
        client->read(0, 0, &value);
        CPPUNIT_ASSERT_EQUAL("item0", toString(&value));
        client->read(0, 1, &value);
        CPPUNIT_ASSERT_EQUAL("item1", toString(&value));
        client->read(0, 2, &value);
        CPPUNIT_ASSERT_EQUAL("item2", toString(&value));
    }
    void test_create_badTable() {
        CPPUNIT_ASSERT_THROW(client->create(4, "", 1),
                             TableDoesntExistException);
    }

    void test_ping() {
        client->ping();
    }

    void test_read_basics() {
        client->create(0, "abcdef", 6);

        Buffer value;
        uint64_t version;
        client->read(0, 0, &value, NULL, &version);
        CPPUNIT_ASSERT_EQUAL(1, version);
        CPPUNIT_ASSERT_EQUAL("abcdef", toString(&value));
    }
    void test_read_badTable() {
        Buffer value;
        CPPUNIT_ASSERT_THROW(client->read(4, 0, &value),
                             TableDoesntExistException);
    }
    void test_read_noSuchObject() {
        Buffer value;
        CPPUNIT_ASSERT_THROW(client->read(0, 5, &value),
                             ObjectDoesntExistException);
    }
    void test_read_rejectRules() {
        client->create(0, "abcdef", 6);

        Buffer value;
        RejectRules rules;
        memset(&rules, 0, sizeof(rules));
        rules.versionNeGiven = true;
        rules.givenVersion = 2;
        uint64_t version;
        CPPUNIT_ASSERT_THROW(client->read(0, 0, &value, &rules, &version),
                             WrongVersionException);
        CPPUNIT_ASSERT_EQUAL(1, version);
    }

    void test_multiRead_basics() {
        client->create(0, "firstVal", 8);
        client->create(0, "secondVal", 9);

        std::vector<MasterClient::ReadObject*> requests;

        Tub<Buffer> val1;
        MasterClient::ReadObject request1(0, 0, &val1);
        request1.status = STATUS_RETRY;
        requests.push_back(&request1);
        Tub<Buffer> val2;
        MasterClient::ReadObject request2(0, 1, &val2);
        request2.status = STATUS_RETRY;
        requests.push_back(&request2);

        client->multiRead(requests);

        CPPUNIT_ASSERT_EQUAL("STATUS_OK", statusToSymbol(request1.status));
        CPPUNIT_ASSERT_EQUAL(1, request1.version);
        CPPUNIT_ASSERT_EQUAL("firstVal", toString(val1.get()));
        CPPUNIT_ASSERT_EQUAL("STATUS_OK", statusToSymbol(request2.status));
        CPPUNIT_ASSERT_EQUAL(2, request2.version);
        CPPUNIT_ASSERT_EQUAL("secondVal", toString(val2.get()));
    }
    void test_multiRead_badTable() {
        client->create(0, "value1", 6);

        std::vector<MasterClient::ReadObject*> requests;

        Tub<Buffer> val1;
        MasterClient::ReadObject request1(0, 0, &val1);
        request1.status = STATUS_RETRY;
        requests.push_back(&request1);

        Tub<Buffer> valError;
        MasterClient::ReadObject requestError(10, 0, &valError);
        requestError.status = STATUS_RETRY;
        requests.push_back(&requestError);

        client->multiRead(requests);

        CPPUNIT_ASSERT_EQUAL("STATUS_OK", statusToSymbol(request1.status));
        CPPUNIT_ASSERT_EQUAL(1, request1.version);
        CPPUNIT_ASSERT_EQUAL("value1", toString(val1.get()));
        CPPUNIT_ASSERT_EQUAL("STATUS_TABLE_DOESNT_EXIST",
                             statusToSymbol(requestError.status));
    }
    void test_multiRead_noSuchObject() {
        client->create(0, "firstVal", 8);
        client->create(0, "secondVal", 9);

        std::vector<MasterClient::ReadObject*> requests;

        Tub<Buffer> val1;
        MasterClient::ReadObject request1(0, 0, &val1);
        request1.status = STATUS_RETRY;
        requests.push_back(&request1);

        Tub<Buffer> valError;
        MasterClient::ReadObject requestError(0, 20, &valError);
        requestError.status = STATUS_RETRY;
        requests.push_back(&requestError);

        Tub<Buffer> val2;
        MasterClient::ReadObject request2(0, 1, &val2);
        request2.status = STATUS_RETRY;
        requests.push_back(&request2);

        client->multiRead(requests);

        CPPUNIT_ASSERT_EQUAL("STATUS_OK", statusToSymbol(request1.status));
        CPPUNIT_ASSERT_EQUAL(1, request1.version);
        CPPUNIT_ASSERT_EQUAL("firstVal", toString(val1.get()));

        CPPUNIT_ASSERT_EQUAL("STATUS_OBJECT_DOESNT_EXIST",
                             statusToSymbol(requestError.status));

        CPPUNIT_ASSERT_EQUAL("STATUS_OK", statusToSymbol(request2.status));
        CPPUNIT_ASSERT_EQUAL(2, request2.version);
        CPPUNIT_ASSERT_EQUAL("secondVal", toString(val2.get()));
    }

    void
    test_detectSegmentRecoveryFailure_success()
    {
        typedef MasterService MS;
        ProtoBuf::ServerList backups;
        ServerListBuilder{backups}
            (ProtoBuf::BACKUP, 123, 87, "mock:host=backup1", MS::REC_REQ_FAILED)
            (ProtoBuf::BACKUP, 123, 88, "mock:host=backup1", MS::REC_REQ_OK)
            (ProtoBuf::BACKUP, 123, 89, "mock:host=backup1", MS::REC_REQ_OK)
            (ProtoBuf::BACKUP, 123, 88, "mock:host=backup1", MS::REC_REQ_OK)
            (ProtoBuf::BACKUP, 123, 87, "mock:host=backup1", MS::REC_REQ_OK)
        ;
        detectSegmentRecoveryFailure(99, 3, backups);
    }

    void
    test_detectSegmentRecoveryFailure_failure()
    {
        typedef MasterService MS;
        ProtoBuf::ServerList backups;
        ServerListBuilder{backups}
            (ProtoBuf::BACKUP, 123, 87, "mock:host=backup1", MS::REC_REQ_FAILED)
            (ProtoBuf::BACKUP, 123, 88, "mock:host=backup1", MS::REC_REQ_OK)
        ;
        CPPUNIT_ASSERT_THROW(detectSegmentRecoveryFailure(99, 3, backups),
                             SegmentRecoveryFailedException);
    }

    static bool
    recoverSegmentFilter(string s)
    {
        return (s == "recoverSegment" || s == "recover" ||
                s == "tabletsRecovered" || s == "setTablets");
    }

    void
    appendTablet(ProtoBuf::Tablets& tablets,
                 uint64_t partitionId,
                 uint32_t tableId,
                 uint64_t start, uint64_t end)
    {
        ProtoBuf::Tablets::Tablet& tablet(*tablets.add_tablet());
        tablet.set_table_id(tableId);
        tablet.set_start_object_id(start);
        tablet.set_end_object_id(end);
        tablet.set_state(ProtoBuf::Tablets::Tablet::RECOVERING);
        tablet.set_user_data(partitionId);
    }

    void
    createTabletList(ProtoBuf::Tablets& tablets)
    {
        appendTablet(tablets, 0, 123, 0, 9);
        appendTablet(tablets, 0, 123, 10, 19);
        appendTablet(tablets, 0, 123, 20, 29);
        appendTablet(tablets, 0, 124, 20, 100);
    }

    void
    test_recover_basics()
    {
        void* segMem = xmemalign(segmentSize, segmentSize);
        Tub<uint64_t> serverId;
        serverId.construct(123);
        BackupManager mgr(coordinator, serverId, 1);
        Segment _(123, 87, segMem, segmentSize, &mgr);
        mgr.sync();

        ProtoBuf::Tablets tablets;
        createTabletList(tablets);
        BackupClient::StartReadingData::Result result;
        BackupClient(transportManager.getSession("mock:host=backup1")).
            startReadingData(123, tablets, &result);

        ProtoBuf::ServerList backups;
        ServerListBuilder{backups}
            (ProtoBuf::BACKUP, 123, 87, "mock:host=backup1");

        TestLog::Enable __(&recoverSegmentFilter);
        client->recover(123, 0, tablets, backups);
        assertMatchesPosixRegex(
            "recover: Starting recovery of 4 tablets on masterId 2 | "
            "setTablets: Now serving tablets: | "
            "setTablets: table:                    0, "
                        "start:                    0, "
                        "end  : 18446744073709551615 | "
            "setTablets: table:                  123, "
                        "start:                    0, "
                        "end  :                    9 | "
            "setTablets: table:                  123, "
                        "start:                   10, "
                        "end  :                   19 | "
            "setTablets: table:                  123, "
                        "start:                   20, "
                        "end  :                   29 | "
            "setTablets: table:                  124, "
                        "start:                   20, "
                        "end  :                  100 | "
            "recover: Recovering master 123, partition 0, 1 hosts | "
            "recover: Starting getRecoveryData from mock:host=backup1 for "
            "segment 87 on channel 0 (initial round of RPCs) | "
            "recover: Waiting on recovery data for segment 87 from "
            "mock:host=backup1 | ",
            TestLog::get());
        assertMatchesPosixRegex(
            "recover: Recovering segment 87 with size 0 | "
            "recoverSegment: recoverSegment 87, ... | ",
            TestLog::get());
        assertMatchesPosixRegex(
            "recover: Checking mock:host=backup1 off the list for 87 | "
            "recover: Checking mock:host=backup1 off the list for 87 | ",
            TestLog::get());
        assertMatchesPosixRegex(
            "recover: set tablet 123 0 9 to locator mock:host=master, id 2 | "
            "recover: set tablet 123 10 19 to locator mock:host=master, id 2 | "
            "recover: set tablet 123 20 29 to locator mock:host=master, id 2 | "
            "recover: set tablet 124 20 100 to locator mock:host=master, "
            "id 2 | "
            "tabletsRecovered: called by masterId 2 with 4 tablets, "
            "5 will entries",
            TestLog::get());

        free(segMem);
    }

    /**
     * Properties checked:
     * 1) At most length of tasks number of RPCs are started initially
     *    even with a longer backup list.
     * 2) Ensures that if a segment is only requested in the initial
     *    round of RPCs once.
     * 3) Ensures that if an entry in the server list is skipped because
     *    another RPC is outstanding for the same segment it is retried
     *    if the earlier RPC fails.
     * 4) Ensures that if an RPC succeeds for one copy of a segment other
     *    RPCs for that segment don't occur.
     * 5) A transport exception at construction time caused that entry
     *    to be skipped and a new entry to be tried immediate, both
     *    during initial RPC starts and following ones.
     */
    void
    test_recover()
    {
        void* segMem = xmemalign(segmentSize, segmentSize);
        Tub<uint64_t> serverId;
        serverId.construct(123);
        BackupManager mgr(coordinator, serverId, 1);
        Segment __(123, 88, segMem, segmentSize, &mgr);
        mgr.sync();

        InMemoryStorage storage2{segmentSize, segmentFrames};
        BackupService backupService2{backupConfig, *storage};
        transport->addService(backupService2, "mock:host=backup2");
        coordinator->enlistServer(BACKUP, "mock:host=backup2");

        ProtoBuf::Tablets tablets;
        createTabletList(tablets);
        BackupClient::StartReadingData::Result result;
        BackupClient(transportManager.getSession("mock:host=backup1")).
            startReadingData(123, tablets, &result);

        ProtoBuf::ServerList backups;
        ServerListBuilder{backups}
            // Started in initial round of RPCs - eventually fails
            (ProtoBuf::BACKUP, 123, 87, "mock:host=backup1")
            // Skipped in initial round of RPCs (prior is in-flight)
            // starts later after failure from earlier entry
            (ProtoBuf::BACKUP, 123, 87, "mock:host=backup2")
            // Started in initial round of RPCs - eventually succeeds
            (ProtoBuf::BACKUP, 123, 88, "mock:host=backup1")
            // Skipped in all rounds of RPCs (prior succeeds)
            (ProtoBuf::BACKUP, 123, 88, "mock:host=backup2")
            // Started in initial round of RPCs - eventually fails
            (ProtoBuf::BACKUP, 123, 89, "mock:host=backup1")
            // Fails to start in initial round of RPCs - bad locator
            (ProtoBuf::BACKUP, 123, 90, "mock:host=backup3")
            // Started in initial round of RPCs - eventually fails
            (ProtoBuf::BACKUP, 123, 91, "mock:host=backup1")
            // Fails to start in later rounds of RPCs - bad locator
            (ProtoBuf::BACKUP, 123, 92, "mock:host=backup4")
            // Started in later rounds of RPCs - eventually fails
            (ProtoBuf::BACKUP, 123, 93, "mock:host=backup1")
        ;

        TestLog::Enable _;
        CPPUNIT_ASSERT_THROW(service->recover(123, 0, backups),
                             SegmentRecoveryFailedException);
        // 1,2,3) 87 was requested from the first server list entry.
        assertMatchesPosixRegex(
            "recover: Starting getRecoveryData from mock:host=backup1 "
            "for segment 87 on channel . (initial round of RPCs)",
            TestLog::get());
        CPPUNIT_ASSERT_EQUAL(MasterService::REC_REQ_FAILED,
                             backups.server(0).user_data());
        // 2,3) 87 was *not* requested a second time in the initial RPC round
        // but was requested later once the first failed.
        assertMatchesPosixRegex(
            "recover: Starting getRecoveryData from mock:host=backup2 "
            "for segment 87 .* (after RPC completion)",
            TestLog::get());
        CPPUNIT_ASSERT_EQUAL(MasterService::REC_REQ_FAILED,
                             backups.server(0).user_data());
        // 1,4) 88 was requested from the third server list entry and
        //      succeeded, which knocks the third and forth entries into
        //      OK status, preventing the launch of the forth entry
        assertMatchesPosixRegex(
            "recover: Starting getRecoveryData from mock:host=backup1 "
            "for segment 88 on channel . (initial round of RPCs)",
            TestLog::get());
        assertMatchesPosixRegex(
            "recover: Checking mock:host=backup1 off the list for 88 | "
            "recover: Checking mock:host=backup2 off the list for 88",
            TestLog::get());
        // 1,4) 88 was requested NOT from the forth server list entry.
        assertNotMatchesPosixRegex(
            "recover: Starting getRecoveryData from mock:host=backup2 "
            "for segment 88 .* (after RPC completion)",
            TestLog::get());
        CPPUNIT_ASSERT_EQUAL(MasterService::REC_REQ_OK,
                             backups.server(2).user_data());
        CPPUNIT_ASSERT_EQUAL(MasterService::REC_REQ_OK,
                             backups.server(3).user_data());
        // 1) Checking to ensure RPCs for 87, 88, 89, 90 went first round
        //    and that 91 got issued in place, first-found due to 90's
        //    bad locator
        assertMatchesPosixRegex(
            "recover: Starting getRecoveryData from mock:host=backup1 "
            "for segment 89 on channel . (initial round of RPCs)",
            TestLog::get());
        CPPUNIT_ASSERT_EQUAL(MasterService::REC_REQ_FAILED,
                             backups.server(4).user_data());
        assertMatchesPosixRegex(
            "recover: Starting getRecoveryData from mock:host=backup3 "
            "for segment 90 on channel . (initial round of RPCs)",
            TestLog::get());
        // 5) Checks bad locators for initial RPCs are handled
        assertMatchesPosixRegex(
            "No transport found for this service locator: mock:host=backup3",
            TestLog::get());
        CPPUNIT_ASSERT_EQUAL(MasterService::REC_REQ_FAILED,
                             backups.server(5).user_data());
        assertMatchesPosixRegex(
            "recover: Starting getRecoveryData from mock:host=backup1 "
            "for segment 91 on channel . (initial round of RPCs)",
            TestLog::get());
        CPPUNIT_ASSERT_EQUAL(MasterService::REC_REQ_FAILED,
                             backups.server(6).user_data());
        assertMatchesPosixRegex(
            "recover: Starting getRecoveryData from mock:host=backup4 "
            "for segment 92 on channel . (after RPC completion)",
            TestLog::get());
        // 5) Checks bad locators for non-initial RPCs are handled
        assertMatchesPosixRegex(
            "No transport found for this service locator: mock:host=backup4",
            TestLog::get());
        CPPUNIT_ASSERT_EQUAL(MasterService::REC_REQ_FAILED,
                             backups.server(7).user_data());
        assertMatchesPosixRegex(
            "recover: Starting getRecoveryData from mock:host=backup1 "
            "for segment 93 on channel . (after RPC completion)",
            TestLog::get());
        CPPUNIT_ASSERT_EQUAL(MasterService::REC_REQ_FAILED,
                             backups.server(8).user_data());

        free(segMem);
    }

    uint32_t
    buildRecoverySegment(void* segmentBuf, uint32_t segmentCapacity,
                         uint64_t tblId, uint64_t objId, uint64_t version,
                         string objContents)
    {
        Segment s(0UL, 0, segmentBuf,
                  downCast<uint32_t>(segmentCapacity), NULL);

        DECLARE_OBJECT(newObject, objContents.length() + 1);
        newObject->id.objectId = objId;
        newObject->id.tableId = tblId;
        newObject->version = version;
        strcpy(newObject->data, objContents.c_str()); // NOLINT fuck off

        uint32_t len = downCast<uint32_t>(objContents.length()) + 1;
        const void *p = s.append(LOG_ENTRY_TYPE_OBJ, newObject,
            newObject->objectLength(len))->userData();
        assert(p != NULL);
        s.close();
        return downCast<uint32_t>(static_cast<const char*>(p) -
                                  static_cast<const char*>(segmentBuf));
    }

    uint32_t
    buildRecoverySegment(void* segmentBuf, uint64_t segmentCapacity,
                         ObjectTombstone *tomb)
    {
        Segment s(0UL, 0, segmentBuf,
                  downCast<uint32_t>(segmentCapacity), NULL);
        const void *p = s.append(LOG_ENTRY_TYPE_OBJTOMB,
            tomb, sizeof(*tomb))->userData();
        assert(p != NULL);
        s.close();
        return downCast<uint32_t>(static_cast<const char*>(p) -
                                  static_cast<const char*>(segmentBuf));
    }

    void
    verifyRecoveryObject(uint64_t tblId, uint64_t objId, string contents)
    {
        Buffer value;
        client->read(downCast<uint32_t>(tblId), objId, &value);
        const char *s = reinterpret_cast<const char *>(
            value.getRange(0, value.getTotalLength()));
        CPPUNIT_ASSERT(strcmp(s, contents.c_str()) == 0);
    }

    void
    test_recoverSegment()
    {
        const uint32_t segLen = 8192;
        void* seg = xmemalign(segLen, segLen);
        uint32_t len; // number of bytes in a recovery segment
        Buffer value;
        bool ret;
        const ObjectTombstone *tomb1 = NULL;
        const ObjectTombstone *tomb2 = NULL;

        ////////////////////////////////////////////////////////////////////
        // For Object recovery there are 3 major cases:
        //  1) Object is in the HashTable, but no corresponding Tombstone.
        //     The recovered obj is only added if the version is newer than
        //     the existing obj.
        //
        //  2) Opposite of 1 above.
        //     The recovered obj is only added if the version is newer than
        //     the tombstone. If so, the tombstone is also discarded.
        //
        //  3) Neither an Object nor Tombstone is present.
        //     The recovered obj is always added.
        ////////////////////////////////////////////////////////////////////

        // Case 1a: Newer object already there; ignore object.
        len = buildRecoverySegment(seg, segLen, 0, 2000, 1, "newer guy");
        service->recoverSegment(0, seg, len);
        verifyRecoveryObject(0, 2000, "newer guy");
        len = buildRecoverySegment(seg, segLen, 0, 2000, 0, "older guy");
        service->recoverSegment(0, seg, len);
        verifyRecoveryObject(0, 2000, "newer guy");

        // Case 1b: Older object already there; replace object.
        len = buildRecoverySegment(seg, segLen, 0, 2001, 0, "older guy");
        service->recoverSegment(0, seg, len);
        verifyRecoveryObject(0, 2001, "older guy");
        len = buildRecoverySegment(seg, segLen, 0, 2001, 1, "newer guy");
        service->recoverSegment(0, seg, len);
        verifyRecoveryObject(0, 2001, "newer guy");

        // Case 2a: Equal/newer tombstone already there; ignore object.
        ObjectTombstone t1(0, 0, 2002, 1);
        LogEntryHandle logTomb1 = service->log.append(LOG_ENTRY_TYPE_OBJTOMB,
            &t1, sizeof(t1), false);
        ret = service->objectMap.replace(logTomb1);
        CPPUNIT_ASSERT_EQUAL(false, ret);
        len = buildRecoverySegment(seg, segLen, 0, 2002, 1, "equal guy");
        service->recoverSegment(0, seg, len);
        len = buildRecoverySegment(seg, segLen, 0, 2002, 0, "older guy");
        service->recoverSegment(0, seg, len);
        CPPUNIT_ASSERT_EQUAL(logTomb1, service->objectMap.lookup(0, 2002));
        service->removeTombstones();
        CPPUNIT_ASSERT_THROW(client->read(0, 2002, &value),
                             ObjectDoesntExistException);

        // Case 2b: Lesser tombstone already there; add object, remove tomb.
        ObjectTombstone t2(0, 0, 2003, 10);
        LogEntryHandle logTomb2 = service->log.append(LOG_ENTRY_TYPE_OBJTOMB,
            &t2, sizeof(t2), false);
        ret = service->objectMap.replace(logTomb2);
        CPPUNIT_ASSERT_EQUAL(false, ret);
        len = buildRecoverySegment(seg, segLen, 0, 2003, 11, "newer guy");
        service->recoverSegment(0, seg, len);
        verifyRecoveryObject(0, 2003, "newer guy");
        CPPUNIT_ASSERT(service->objectMap.lookup(0, 2003) != NULL);
        CPPUNIT_ASSERT(service->objectMap.lookup(0, 2003) != logTomb1);
        CPPUNIT_ASSERT(service->objectMap.lookup(0, 2003) != logTomb2);
        service->removeTombstones();

        // Case 3: No tombstone, no object. Recovered object always added.
        CPPUNIT_ASSERT_EQUAL(NULL, service->objectMap.lookup(0, 2004));
        len = buildRecoverySegment(seg, segLen, 0, 2004, 0, "only guy");
        service->recoverSegment(0, seg, len);
        verifyRecoveryObject(0, 2004, "only guy");

        ////////////////////////////////////////////////////////////////////
        // For ObjectTombstone recovery there are the same 3 major cases:
        //  1) Object is in  the HashTable, but no corresponding Tombstone.
        //     The recovered tomb is only added if the version is equal to
        //     or greater than the object. If so, the object is purged.
        //
        //  2) Opposite of 1 above.
        //     The recovered tomb is only added if the version is newer than
        //     the current tombstone. If so, the old tombstone is discarded.
        //
        //  3) Neither an Object nor Tombstone is present.
        //     The recovered tombstone is always added.
        ////////////////////////////////////////////////////////////////////

        // Case 1a: Newer object already there; ignore tombstone.
        len = buildRecoverySegment(seg, segLen, 0, 2005, 1, "newer guy");
        service->recoverSegment(0, seg, len);
        ObjectTombstone t3(0, 0, 2005, 0);
        len = buildRecoverySegment(seg, segLen, &t3);
        service->recoverSegment(0, seg, len);
        verifyRecoveryObject(0, 2005, "newer guy");

        // Case 1b: Equal/older object already there; discard and add tombstone.
        len = buildRecoverySegment(seg, segLen, 0, 2006, 0, "equal guy");
        service->recoverSegment(0, seg, len);
        verifyRecoveryObject(0, 2006, "equal guy");
        ObjectTombstone t4(0, 0, 2006, 0);
        len = buildRecoverySegment(seg, segLen, &t4);
        service->recoverSegment(0, seg, len);
        service->removeTombstones();
        CPPUNIT_ASSERT_EQUAL(NULL, service->objectMap.lookup(0, 2006));
        CPPUNIT_ASSERT_THROW(client->read(0, 2006, &value),
                             ObjectDoesntExistException);

        len = buildRecoverySegment(seg, segLen, 0, 2007, 0, "older guy");
        service->recoverSegment(0, seg, len);
        verifyRecoveryObject(0, 2007, "older guy");
        ObjectTombstone t5(0, 0, 2007, 1);
        len = buildRecoverySegment(seg, segLen, &t5);
        service->recoverSegment(0, seg, len);
        service->removeTombstones();
        CPPUNIT_ASSERT_EQUAL(NULL, service->objectMap.lookup(0, 2007));
        CPPUNIT_ASSERT_THROW(client->read(0, 2007, &value),
                             ObjectDoesntExistException);

        // Case 2a: Newer tombstone already there; ignore.
        ObjectTombstone t6(0, 0, 2008, 1);
        len = buildRecoverySegment(seg, segLen, &t6);
        service->recoverSegment(0, seg, len);
        tomb1 = service->objectMap.lookup(0, 2008)->userData<ObjectTombstone>();
        CPPUNIT_ASSERT(tomb1 != NULL);
        CPPUNIT_ASSERT_EQUAL(1, tomb1->objectVersion);
        ObjectTombstone t7(0, 0, 2008, 0);
        len = buildRecoverySegment(seg, segLen, &t7);
        service->recoverSegment(0, seg, len);
        tomb2 = service->objectMap.lookup(0, 2008)->userData<ObjectTombstone>();
        CPPUNIT_ASSERT_EQUAL(tomb1, tomb2);

        // Case 2b: Older tombstone already there; replace.
        ObjectTombstone t8(0, 0, 2009, 0);
        len = buildRecoverySegment(seg, segLen, &t8);
        service->recoverSegment(0, seg, len);
        tomb1 = service->objectMap.lookup(0, 2009)->userData<ObjectTombstone>();
        CPPUNIT_ASSERT(tomb1 != NULL);
        CPPUNIT_ASSERT_EQUAL(0, tomb1->objectVersion);
        ObjectTombstone t9(0, 0, 2009, 1);
        len = buildRecoverySegment(seg, segLen, &t9);
        service->recoverSegment(0, seg, len);
        tomb2 = service->objectMap.lookup(0, 2009)->userData<ObjectTombstone>();
        CPPUNIT_ASSERT_EQUAL(1, tomb2->objectVersion);

        // Case 3: No tombstone, no object. Recovered tombstone always added.
        CPPUNIT_ASSERT_EQUAL(NULL, service->objectMap.lookup(0, 2010));
        ObjectTombstone t10(0, 0, 2010, 0);
        len = buildRecoverySegment(seg, segLen, &t10);
        service->recoverSegment(0, seg, len);
        CPPUNIT_ASSERT(service->objectMap.lookup(0, 2010) != NULL);
        CPPUNIT_ASSERT_EQUAL(LOG_ENTRY_TYPE_OBJTOMB,
            service->objectMap.lookup(0, 2010)->type());
        CPPUNIT_ASSERT_EQUAL(0, memcmp(&t10, service->objectMap.lookup(
            0, 2010)->userData(), sizeof(t10)));
    }

    void test_remove_basics() {
        client->create(0, "item0", 5);

        uint64_t version;
        client->remove(0, 0, NULL, &version);
        CPPUNIT_ASSERT_EQUAL(1, version);

        Buffer value;
        CPPUNIT_ASSERT_THROW(client->read(0, 0, &value),
                             ObjectDoesntExistException);
    }
    void test_remove_badTable() {
        CPPUNIT_ASSERT_THROW(client->remove(4, 0),
                             TableDoesntExistException);
    }
    void test_remove_rejectRules() {
        client->create(0, "item0", 5);

        RejectRules rules;
        memset(&rules, 0, sizeof(rules));
        rules.versionNeGiven = true;
        rules.givenVersion = 2;
        uint64_t version;
        CPPUNIT_ASSERT_THROW(client->remove(0, 0, &rules, &version),
                             WrongVersionException);
        CPPUNIT_ASSERT_EQUAL(1, version);
    }
    void test_remove_objectAlreadyDeletedRejectRules() {
        RejectRules rules;
        memset(&rules, 0, sizeof(rules));
        rules.doesntExist = true;
        uint64_t version;
        CPPUNIT_ASSERT_THROW(client->remove(0, 0, &rules, &version),
                             ObjectDoesntExistException);
        CPPUNIT_ASSERT_EQUAL(VERSION_NONEXISTENT, version);
    }
    void test_remove_objectAlreadyDeleted() {
        uint64_t version;
        client->remove(0, 1, NULL, &version);
        CPPUNIT_ASSERT_EQUAL(VERSION_NONEXISTENT, version);
        client->create(0, "abcdef", 6);
        client->remove(0, 0);
        client->remove(0, 0, NULL, &version);
        CPPUNIT_ASSERT_EQUAL(VERSION_NONEXISTENT, version);
    }

    void test_setTablets() {

        std::unique_ptr<Table> table1(new Table(1));
        uint64_t addrTable1 = reinterpret_cast<uint64_t>(table1.get());
        std::unique_ptr<Table> table2(new Table(2));
        uint64_t addrTable2 = reinterpret_cast<uint64_t>(table2.get());

        { // clear out the tablets through client
            ProtoBuf::Tablets newTablets;
            client->setTablets(newTablets);
            CPPUNIT_ASSERT_EQUAL("", service->tablets.ShortDebugString());
        }

        { // set t1 and t2 directly
            ProtoBuf::Tablets_Tablet& t1(*service->tablets.add_tablet());
            t1.set_table_id(1);
            t1.set_start_object_id(0);
            t1.set_end_object_id(1);
            t1.set_state(ProtoBuf::Tablets_Tablet_State_NORMAL);
            t1.set_user_data(reinterpret_cast<uint64_t>(table1.release()));

            ProtoBuf::Tablets_Tablet& t2(*service->tablets.add_tablet());
            t2.set_table_id(2);
            t2.set_start_object_id(0);
            t2.set_end_object_id(1);
            t2.set_state(ProtoBuf::Tablets_Tablet_State_NORMAL);
            t2.set_user_data(reinterpret_cast<uint64_t>(table2.release()));

            CPPUNIT_ASSERT_EQUAL(format(
                "tablet { table_id: 1 start_object_id: 0 end_object_id: 1 "
                    "state: NORMAL user_data: %lu } "
                "tablet { table_id: 2 start_object_id: 0 end_object_id: 1 "
                    "state: NORMAL user_data: %lu }",
                addrTable1, addrTable2),
                                 service->tablets.ShortDebugString());
        }

        { // set t2, t2b, and t3 through client
            ProtoBuf::Tablets newTablets;

            ProtoBuf::Tablets_Tablet& t2(*newTablets.add_tablet());
            t2.set_table_id(2);
            t2.set_start_object_id(0);
            t2.set_end_object_id(1);
            t2.set_state(ProtoBuf::Tablets_Tablet_State_NORMAL);

            ProtoBuf::Tablets_Tablet& t2b(*newTablets.add_tablet());
            t2b.set_table_id(2);
            t2b.set_start_object_id(2);
            t2b.set_end_object_id(3);
            t2b.set_state(ProtoBuf::Tablets_Tablet_State_NORMAL);

            ProtoBuf::Tablets_Tablet& t3(*newTablets.add_tablet());
            t3.set_table_id(3);
            t3.set_start_object_id(0);
            t3.set_end_object_id(1);
            t3.set_state(ProtoBuf::Tablets_Tablet_State_NORMAL);

            client->setTablets(newTablets);

            CPPUNIT_ASSERT_EQUAL(format(
                "tablet { table_id: 2 start_object_id: 0 end_object_id: 1 "
                    "state: NORMAL user_data: %lu } "
                "tablet { table_id: 2 start_object_id: 2 end_object_id: 3 "
                    "state: NORMAL user_data: %lu } "
                "tablet { table_id: 3 start_object_id: 0 end_object_id: 1 "
                    "state: NORMAL user_data: %lu }",
                addrTable2, addrTable2,
                service->tablets.tablet(2).user_data()),
                                 service->tablets.ShortDebugString());
        }
    }

    void test_write() {
        Buffer value;
        uint64_t version;
        client->write(0, 3, "item0", 5, NULL, &version);
        CPPUNIT_ASSERT_EQUAL(1, version);
        client->read(0, 3, &value, NULL, &version);
        CPPUNIT_ASSERT_EQUAL("item0", toString(&value));
        CPPUNIT_ASSERT_EQUAL(1, version);

        client->write(0, 3, "item0-v2", 8, NULL, &version);
        CPPUNIT_ASSERT_EQUAL(2, version);
        client->read(0, 3, &value);
        CPPUNIT_ASSERT_EQUAL("item0-v2", toString(&value));

        client->write(0, 3, "item0-v3", 8, NULL, &version);
        CPPUNIT_ASSERT_EQUAL(3, version);
        client->read(0, 3, &value, NULL, &version);
        CPPUNIT_ASSERT_EQUAL("item0-v3", toString(&value));
        CPPUNIT_ASSERT_EQUAL(3, version);
    }
    void test_write_rejectRules() {
        RejectRules rules;
        memset(&rules, 0, sizeof(rules));
        rules.doesntExist = true;
        uint64_t version;
        CPPUNIT_ASSERT_THROW(client->write(0, 3, "item0", 5, &rules, &version),
                             ObjectDoesntExistException);
        CPPUNIT_ASSERT_EQUAL(VERSION_NONEXISTENT, version);
    }

    void test_getTable() {
        // Table exists.
        CPPUNIT_ASSERT_NO_THROW(service->getTable(0, 0));

        // Table doesn't exist.
        Status status = Status(0);
        try {
            service->getTable(1000, 0);
        } catch (TableDoesntExistException& e) {
            status = e.status;
        }
        CPPUNIT_ASSERT_EQUAL(1, status);
    }

    void test_rejectOperation() {
        RejectRules empty, rules;
        memset(&empty, 0, sizeof(empty));

        // Fail: object doesn't exist.
        rules = empty;
        rules.doesntExist = 1;
        CPPUNIT_ASSERT_THROW(
                service->rejectOperation(&rules, VERSION_NONEXISTENT),
                ObjectDoesntExistException);

        // Succeed: object doesn't exist.
        rules = empty;
        rules.exists = rules.versionLeGiven = rules.versionNeGiven = 1;
        CPPUNIT_ASSERT_NO_THROW(
                service->rejectOperation(&rules, VERSION_NONEXISTENT));

        // Fail: object exists.
        rules = empty;
        rules.exists = 1;
        CPPUNIT_ASSERT_THROW(service->rejectOperation(&rules, 2),
                             ObjectExistsException);

        // versionLeGiven.
        rules = empty;
        rules.givenVersion = 0x400000001;
        rules.versionLeGiven = 1;
        CPPUNIT_ASSERT_THROW(
                service->rejectOperation(&rules, 0x400000000),
                WrongVersionException);
        CPPUNIT_ASSERT_THROW(
                service->rejectOperation(&rules, 0x400000001),
                WrongVersionException);
        CPPUNIT_ASSERT_NO_THROW(
                service->rejectOperation(&rules, 0x400000002));

        // versionNeGiven.
        rules = empty;
        rules.givenVersion = 0x400000001;
        rules.versionNeGiven = 1;
        CPPUNIT_ASSERT_THROW(
                service->rejectOperation(&rules, 0x400000000),
                WrongVersionException);
        CPPUNIT_ASSERT_NO_THROW(
                service->rejectOperation(&rules, 0x400000001));
        CPPUNIT_ASSERT_THROW(
                service->rejectOperation(&rules, 0x400000002),
                WrongVersionException);
    }

    DISALLOW_COPY_AND_ASSIGN(MasterServiceTest);
};
CPPUNIT_TEST_SUITE_REGISTRATION(MasterServiceTest);


/**
 * Unit tests for Master::_recover.
 */
class MasterRecoverTest : public CppUnit::TestFixture {

    CPPUNIT_TEST_SUITE(MasterRecoverTest);
    CPPUNIT_TEST(test_recover);
    CPPUNIT_TEST(test_recover_failedToRecoverAll);
    CPPUNIT_TEST_SUITE_END();

    BackupService* backupService1;
    BackupService* backupService2;
    CoordinatorClient* coordinator;
    CoordinatorService* coordinatorService;
    BackupService::Config* config;
    const uint32_t segmentSize;
    const uint32_t segmentFrames;
    BackupStorage* storage1;
    BackupStorage* storage2;
    BindTransport* transport;

  public:
    MasterRecoverTest()
        : backupService1()
        , backupService2()
        , coordinator()
        , coordinatorService()
        , config()
        , segmentSize(1 << 16)
        , segmentFrames(2)
        , storage1()
        , storage2()
        , transport()
    {
    }

    void
    setUp(bool enlist)
    {
        if (!enlist)
            tearDown();

        transport = new BindTransport;
        transportManager.registerMock(transport);

        config = new BackupService::Config;
        config->coordinatorLocator = "mock:host=coordinator";

        coordinatorService = new CoordinatorService;
        transport->addService(*coordinatorService, config->coordinatorLocator);

        coordinator = new CoordinatorClient(config->coordinatorLocator.c_str());

        storage1 = new InMemoryStorage(segmentSize, segmentFrames);
        storage2 = new InMemoryStorage(segmentSize, segmentFrames);

        backupService1 = new BackupService(*config, *storage1);
        backupService2 = new BackupService(*config, *storage2);

        transport->addService(*backupService1, "mock:host=backup1");
        transport->addService(*backupService2, "mock:host=backup2");

        if (enlist) {
            coordinator->enlistServer(BACKUP, "mock:host=backup1");
            coordinator->enlistServer(BACKUP, "mock:host=backup2");
        }
    }

    void
    setUp()
    {
        setUp(true);
    }


    void
    tearDown()
    {
        delete backupService2;
        delete backupService1;
        delete storage2;
        delete storage1;
        delete coordinator;
        delete coordinatorService;
        delete config;
        transportManager.unregisterMock();
        delete transport;
        CPPUNIT_ASSERT_EQUAL(0,
            BackupStorage::Handle::resetAllocatedHandlesCount());
    }
    static bool
    recoverSegmentFilter(string s)
    {
        return (s == "recoverSegment" || s == "recover");
    }

    MasterService*
    createMasterService()
    {
        ServerConfig config;
        config.coordinatorLocator = "mock:host=coordinator";
        MasterService::sizeLogAndHashTable("64", "8", &config);
        return new MasterService(config, coordinator, 2);
    }

    void
    appendTablet(ProtoBuf::Tablets& tablets,
                 uint64_t partitionId,
                 uint32_t tableId,
                 uint64_t start, uint64_t end)
    {
        ProtoBuf::Tablets::Tablet& tablet(*tablets.add_tablet());
        tablet.set_table_id(tableId);
        tablet.set_start_object_id(start);
        tablet.set_end_object_id(end);
        tablet.set_state(ProtoBuf::Tablets::Tablet::RECOVERING);
        tablet.set_user_data(partitionId);
    }

    void
    createTabletList(ProtoBuf::Tablets& tablets)
    {
        appendTablet(tablets, 0, 123, 0, 9);
        appendTablet(tablets, 0, 123, 10, 19);
        appendTablet(tablets, 0, 123, 20, 29);
        appendTablet(tablets, 0, 124, 20, 100);
    }

    void
    test_recover()
    {
        boost::scoped_ptr<MasterService> master(createMasterService());

        // Give them a name so that freeSegment doesn't get called on
        // destructor until after the test.
        void* segMem1 = xmemalign(segmentSize, segmentSize);
        Tub<uint64_t> serverId;
        serverId.construct(99);
        BackupManager mgr(coordinator, serverId, 2);
        Segment s1(99, 87, segMem1, segmentSize, &mgr);
        s1.close();
        void* segMem2 = xmemalign(segmentSize, segmentSize);
        Segment s2(99, 88, segMem2, segmentSize, &mgr);
        s2.close();

        ProtoBuf::Tablets tablets;
        createTabletList(tablets);

        {
            BackupClient::StartReadingData::Result result;
            BackupClient(transportManager.getSession("mock:host=backup1"))
                .startReadingData(99, tablets, &result);
        }
        {
            BackupClient::StartReadingData::Result result;
            BackupClient(transportManager.getSession("mock:host=backup2"))
                .startReadingData(99, tablets, &result);
        }

        ProtoBuf::ServerList backups;
        ServerListBuilder{backups}
            (ProtoBuf::BACKUP, 99, 87, "mock:host=backup1")
            (ProtoBuf::BACKUP, 99, 88, "mock:host=backup1")
            (ProtoBuf::BACKUP, 99, 88, "mock:host=backup2")
        ;

        MockRandom __(1); // triggers deterministic rand().
        TestLog::Enable _(&recoverSegmentFilter);
        master->recover(99, 0, backups);
        CPPUNIT_ASSERT_EQUAL(0, TestLog::get().find(
            "recover: Recovering master 99, partition 0, 3 hosts"));
        CPPUNIT_ASSERT(string::npos != TestLog::get().find(
            "recoverSegment: Segment 88 replay complete"));
        CPPUNIT_ASSERT(string::npos != TestLog::get().find(
            "recoverSegment: Segment 87 replay complete"));

        free(segMem1);
        free(segMem2);
    }

    void
    test_recover_failedToRecoverAll()
    {
        boost::scoped_ptr<MasterService> master(createMasterService());

        ProtoBuf::Tablets tablets;
        ProtoBuf::ServerList backups;
        ServerListBuilder{backups}
            (ProtoBuf::BACKUP, 99, 87, "mock:host=backup1")
            (ProtoBuf::BACKUP, 99, 88, "mock:host=backup1")
        ;

        MockRandom __(1); // triggers deterministic rand().
        TestLog::Enable _(&recoverSegmentFilter);
        CPPUNIT_ASSERT_THROW(
            master->recover(99, 0, backups),
            SegmentRecoveryFailedException);
        string log = TestLog::get();
        CPPUNIT_ASSERT_EQUAL(
            "recover: Recovering master 99, partition 0, 2 hosts | "
            "recover: Starting getRecoveryData from mock:host=backup1 "
            "for segment 87 on channel 0 (initial round of RPCs) | "
            "recover: Starting getRecoveryData from mock:host=backup1 "
            "for segment 88 on channel 1 (initial round of RPCs) | "
            "recover: Waiting on recovery data for segment 87 from "
            "mock:host=backup1 | "
            "recover: getRecoveryData failed on mock:host=backup1, "
            "trying next backup; failure was: bad segment id",
            log.substr(0, log.find(" thrown at")));
    }
    DISALLOW_COPY_AND_ASSIGN(MasterRecoverTest);
};
CPPUNIT_TEST_SUITE_REGISTRATION(MasterRecoverTest);

}  // namespace RAMCloud
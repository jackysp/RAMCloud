/* Copyright (c) 2010 Stanford University
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

#include "RamCloudClient.h"
#include "TransportManager.h"

namespace RAMCloud {

// Default RejectRules to use if none are provided by the caller.
RejectRules defaultRejectRules;

/**
 * Construct a RamCloudClient for a particular service: opens a connection with the
 * service.
 *
 * \param serviceLocator
 *      The service locator for the master (later this will be for the
 *      coordinator).
 *      See \ref ServiceLocatorStrings.
 * \exception CouldntConnectException
 *      Couldn't connect to the server.
 */
RamCloudClient::RamCloudClient(const char* serviceLocator)
        : session(transportManager.getSession(serviceLocator)),
          objectFinder(session) { }

/**
 * Create a new object in a table, with an id assigned by the server.
 *
 * \param tableId
 *      The table in which the new object is to be created (return
 *      value from a previous call to openTable).
 * \param buf
 *      Address of the first byte of the contents for the new object;
 *      must contain at least length bytes.
 * \param length
 *      Size in bytes of the new object.
 * \param[out] version
 *      If non-NULL, the version number of the new object is returned
 *      here; guaranteed to be greater than that of any previous
 *      object that used the same id in the same table.
 *      
 * \return
 *      The identifier for the new object: unique within the table
 *      and guaranteed not to be in use already. Generally, servers
 *      choose ids sequentially starting at 1 (but they may need
 *      to skip over ids previously created using \c write).
 *
 * \exception InternalError
 */
uint64_t
RamCloudClient::create(uint32_t tableId, const void* buf, uint32_t length,
        uint64_t* version)
{
    Buffer req, resp;
    CreateRpc::Request& reqHdr(allocHeader<CreateRpc>(req));
    reqHdr.tableId = tableId;
    reqHdr.length = length;
    Buffer::Chunk::appendToBuffer(&req, buf, length);
    Transport::SessionRef master(objectFinder.lookupHead(tableId));
    const CreateRpc::Response& respHdr(sendRecv<CreateRpc>(master, req, resp));
    if (version != NULL)
        *version = respHdr.version;
    checkStatus();
    return respHdr.id;
}

/**
 * Create a new table.
 *
 * \param name
 *      Name for the new table (NULL-terminated string).
 *
 * \exception NoTableSpaceException
 * \exception InternalError
 */
void
RamCloudClient::createTable(const char* name)
{
    Buffer req, resp;
    uint32_t length = strlen(name) + 1;
    CreateTableRpc::Request& reqHdr(allocHeader<CreateTableRpc>(req));
    reqHdr.nameLength = length;
    memcpy(new(&req, APPEND) char[length], name, length);
    sendRecv<CreateTableRpc>(session, req, resp);
    checkStatus();
}

/**
 * Delete a table.
 *
 * All objects in the table are implicitly deleted, along with any
 * other information associated with the table (such as, someday,
 * indexes).  If the table does not currently exist than the operation
 * returns successfully without actually doing anything.
 *
 * \param name
 *      Name of the table to delete (NULL-terminated string).
 *  
 * \exception InternalError
 */
void
RamCloudClient::dropTable(const char* name)
{
    Buffer req, resp;
    uint32_t length = strlen(name) + 1;
    DropTableRpc::Request& reqHdr(allocHeader<DropTableRpc>(req));
    reqHdr.nameLength = length;
    memcpy(new(&req, APPEND) char[length], name, length);
    sendRecv<DropTableRpc>(session, req, resp);
    checkStatus();
}

/**
 * Look up a table by name and return a small integer handle that
 * can be used to access the table.
 *
 * \param name
 *      Name of the desired table (NULL-terminated string).
 *      
 * \return
 *      The return value is an identifier for the table; this is used
 *      instead of the table's name for most RAMCloud operations
 *      involving the table.
 *
 * \exception TableDoesntExistException
 * \exception InternalError
 */
uint32_t
RamCloudClient::openTable(const char* name)
{
    Buffer req, resp;
    uint32_t length = strlen(name) + 1;
    OpenTableRpc::Request& reqHdr(allocHeader<OpenTableRpc>(req));
    reqHdr.nameLength = length;
    memcpy(new(&req, APPEND) char[length], name, length);
    const OpenTableRpc::Response& respHdr(
        sendRecv<OpenTableRpc>(session, req, resp));
    checkStatus();
    return respHdr.tableId;
}

/**
 * Test that a server exists and is responsive.
 *
 * This operation issues a no-op RPC request, which causes
 * communication with the given server but doesn't actually do
 * anything on the server.
 *
 * \exception InternalError
 */
void
RamCloudClient::ping()
{
    Buffer req, resp;
    allocHeader<PingRpc>(req);
    sendRecv<PingRpc>(session, req, resp);
    checkStatus();
}

/**
 * Read the current contents of an object.
 *
 * \param tableId
 *      The table containing the desired object (return value from
 *      a previous call to openTable).
 * \param id
 *      Identifier within tableId of the object to be read.
 * \param[out] value
 *      After a successful return, this Buffer will hold the
 *      contents of the desired object.
 * \param rejectRules
 *      If non-NULL, specifies conditions under which the read
 *      should be aborted with an error.
 * \param[out] version
 *      If non-NULL, the version number of the object is returned
 *      here.
 *
 * \exception RejectRulesException
 * \exception InternalError
 */
void
RamCloudClient::read(uint32_t tableId, uint64_t id, Buffer* value,
        const RejectRules* rejectRules, uint64_t* version)
{
    Buffer req;
    uint32_t length;
    ReadRpc::Request& reqHdr(allocHeader<ReadRpc>(req));
    reqHdr.id = id;
    reqHdr.tableId = tableId;
    reqHdr.rejectRules = rejectRules ? *rejectRules : defaultRejectRules;
    Transport::SessionRef master(objectFinder.lookup(tableId, id));
    const ReadRpc::Response& respHdr(sendRecv<ReadRpc>(master, req, *value));
    if (version != NULL)
        *version = respHdr.version;
    length = respHdr.length;

    // Truncate the response Buffer so that it consists of nothing
    // but the object data.
    value->truncateFront(sizeof(respHdr));
    uint32_t extra = value->getTotalLength() - length;
    if (extra > 0)
        value->truncateEnd(extra);
    checkStatus();
}

/**
 * Delete an object from a table. If the object does not currently exist
 * and no rejectRules match, then the operation succeeds without doing
 * anything.
 *
 * \param tableId
 *      The table containing the object to be deleted (return value from
 *      a previous call to openTable).
 * \param id
 *      Identifier within tableId of the object to be deleted.
 * \param rejectRules
 *      If non-NULL, specifies conditions under which the delete
 *      should be aborted with an error.  If NULL, the object is
 *      deleted unconditionally.
 * \param[out] version
 *      If non-NULL, the version number of the object (prior to
 *      deletion) is returned here.  If the object didn't exist
 *      then 0 will be returned.
 *
 * \exception RejectRulesException
 * \exception InternalError
 */
void
RamCloudClient::remove(uint32_t tableId, uint64_t id,
        const RejectRules* rejectRules, uint64_t* version)
{
    Buffer req, resp;
    RemoveRpc::Request& reqHdr(allocHeader<RemoveRpc>(req));
    reqHdr.id = id;
    reqHdr.tableId = tableId;
    reqHdr.rejectRules = rejectRules ? *rejectRules : defaultRejectRules;
    Transport::SessionRef master(objectFinder.lookup(tableId, id));
    const RemoveRpc::Response& respHdr(sendRecv<RemoveRpc>(master, req, resp));
    if (version != NULL)
        *version = respHdr.version;
    checkStatus();
}

/**
 * Write a specific object in a table; overwrite any existing
 * object, or create a new object if none existed.
 *
 * \param tableId
 *      The table containing the desired object (return value from a
 *      previous call to openTable).
 * \param id
 *      Identifier within tableId of the object to be written; may or
 *      may not refer to an existing object.
 * \param buf
 *      Address of the first byte of the new contents for the object;
 *      must contain at least length bytes.
 * \param length
 *      Size in bytes of the new contents for the object.
 * \param rejectRules
 *      If non-NULL, specifies conditions under which the write
 *      should be aborted with an error. NULL means the object should
 *      be written unconditionally.
 * \param[out] version
 *      If non-NULL, the version number of the object is returned
 *      here. If the operation was successful this will be the new
 *      version for the object; if this object has ever existed
 *      previously the new version is guaranteed to be greater than
 *      any previous version of the object. If the operation failed
 *      then the version number returned is the current version of
 *      the object, or 0 if the object does not exist.
 *
 * \exception RejectRulesException
 * \exception InternalError
 */
void
RamCloudClient::write(uint32_t tableId, uint64_t id,
                      const void* buf, uint32_t length,
                      const RejectRules* rejectRules, uint64_t* version)
{
    Buffer req, resp;
    WriteRpc::Request& reqHdr(allocHeader<WriteRpc>(req));
    reqHdr.id = id;
    reqHdr.tableId = tableId;
    reqHdr.length = length;
    reqHdr.rejectRules = rejectRules ? *rejectRules : defaultRejectRules;
    Buffer::Chunk::appendToBuffer(&req, buf, length);
    Transport::SessionRef master(objectFinder.lookup(tableId, id));
    const WriteRpc::Response& respHdr(sendRecv<WriteRpc>(master, req, resp));
    if (version != NULL)
        *version = respHdr.version;
    checkStatus();
}

}  // namespace RAMCloud

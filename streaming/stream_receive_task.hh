/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Modified by Cloudius Systems.
 * Copyright 2015 Cloudius Systems.
 */

#pragma once

#include "utils/UUID.hh"
#include "streaming/stream_task.hh"
#include "streaming/messages/incoming_file_message.hh"
#include <memory>

namespace streaming {

class stream_session;

/**
 * Task that manages receiving files for the session for certain ColumnFamily.
 */
class stream_receive_task : public stream_task {
private:
    // number of files to receive
    int total_files;
    // total size of files to receive
    long total_size;

    // true if task is done (either completed or aborted)
    bool done = false;

    // holds references to SSTables received
    // protected Collection<SSTableWriter> sstables;
public:
    stream_receive_task(shared_ptr<stream_session> _session, UUID _cf_id, int _total_files, long _total_size);
    ~stream_receive_task();

    /**
     * Process received file.
     *
     * @param sstable SSTable file received.
     */
    void received(messages::incoming_file_message message) {
        // TODO: Iterate message.mr and write each mutation
#if 0
        if (done)
            return;

        assert cfId.equals(sstable.metadata.cfId);

        sstables.add(sstable);
        if (sstables.size() == totalFiles)
        {
            done = true;
            executor.submit(new OnCompletionRunnable(this));
        }
#endif
    }

    virtual int get_total_number_of_files() override {
        return total_files;
    }

    virtual long get_total_size() override {
        return total_size;
    }

#if 0
    private static class OnCompletionRunnable implements Runnable
    {
        private final StreamReceiveTask task;

        public OnCompletionRunnable(StreamReceiveTask task)
        {
            this.task = task;
        }

        public void run()
        {
            Pair<String, String> kscf = Schema.instance.getCF(task.cfId);
            if (kscf == null)
            {
                // schema was dropped during streaming
                for (SSTableWriter writer : task.sstables)
                    writer.abort();
                task.sstables.clear();
                return;
            }
            ColumnFamilyStore cfs = Keyspace.open(kscf.left).getColumnFamilyStore(kscf.right);

            File lockfiledir = cfs.directories.getWriteableLocationAsFile(task.sstables.size() * 256);
            if (lockfiledir == null)
                throw new IOError(new IOException("All disks full"));
            StreamLockfile lockfile = new StreamLockfile(lockfiledir, UUID.randomUUID());
            lockfile.create(task.sstables);
            List<SSTableReader> readers = new ArrayList<>();
            for (SSTableWriter writer : task.sstables)
                readers.add(writer.closeAndOpenReader());
            lockfile.delete();
            task.sstables.clear();

            if (!SSTableReader.acquireReferences(readers))
                throw new AssertionError("We shouldn't fail acquiring a reference on a sstable that has just been transferred");
            try
            {
                // add sstables and build secondary indexes
                cfs.addSSTables(readers);
                cfs.indexManager.maybeBuildSecondaryIndexes(readers, cfs.indexManager.allIndexesNames());
            }
            finally
            {
                SSTableReader.releaseReferences(readers);
            }

            task.session.taskCompleted(task);
        }
    }
#endif

    /**
     * Abort this task.
     * If the task already received all files and
     * {@link org.apache.cassandra.streaming.StreamReceiveTask.OnCompletionRunnable} task is submitted,
     * then task cannot be aborted.
     */
    virtual void abort() override {
#if 0
        if (done)
            return;

        done = true;
        for (SSTableWriter writer : sstables)
            writer.abort();
        sstables.clear();
#endif
    }
};

} // namespace streaming

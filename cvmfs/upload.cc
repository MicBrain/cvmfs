/**
 * This file is part of the CernVM File System.
 */

#include "upload.h"

#include <vector>

#include "upload_local.h"
#include "upload_riak.h"

using namespace upload;

Spooler::SpoolerDefinition::SpoolerDefinition(
                                    const std::string& definition_string,
                                    const int          max_pending_jobs) :
  max_pending_jobs(max_pending_jobs),
  valid_(false)
{
  // split the spooler definition into spooler driver and pipe definitions
  std::vector<std::string> components = SplitString(definition_string, ',');
  if (components.size() != 3) {
    LogCvmfs(kLogSpooler, kLogStderr, "Invalid spooler definition");
    return;
  }

  // split the spooler driver definition into name and config part
  std::vector<std::string> upstream = SplitString(components[0], ':', 2);
  if (upstream.size() != 2) {
    LogCvmfs(kLogSpooler, kLogStderr, "Invalid spooler driver");
    return;
  }

  // recognize and configure the spooler driver
  if (upstream[0] == "local") {
    driver_type = Local;
  } else if (upstream[0] == "riak") {
    driver_type = Riak;
  } else {
    LogCvmfs(kLogSpooler, kLogStderr, "unknown spooler driver: %s",
      upstream[0].c_str());
    return;
  }

  spooler_description = upstream[1];

  // save named pipe paths and validate this SpoolerDefinition
  paths_out_pipe  = components[1];
  digests_in_pipe = components[2];
  valid_ = true;
}

Spooler* Spooler::Construct(const std::string &spooler_description,
                            const int          max_pending_jobs) {
  // parse the spooler description string
  SpoolerDefinition spooler_definition(spooler_description, max_pending_jobs);
  assert (spooler_definition.IsValid());

  // create a concrete Spooler object dependent on the parsed definition
  Spooler *spooler = NULL;
  switch (spooler_definition.driver_type) {
    case SpoolerDefinition::Local:
      spooler = new SpoolerImpl<LocalPushWorker>(spooler_definition);
      break;

    case SpoolerDefinition::Riak:
      spooler = new SpoolerImpl<RiakPushWorker>(spooler_definition);
      break;

    default:
      LogCvmfs(kLogSpooler, kLogStderr, "invalid spooler definition");
  }

  assert (spooler != NULL);

  // initialize the spooler and return it to the user
  if (!spooler->Initialize()) {
    delete spooler;
    return NULL;
  }
  return spooler;
}


Spooler::Spooler(const SpoolerDefinition &spooler_definition) :
  callback_(NULL),
  spooler_definition_(spooler_definition),
  transaction_ends_(false),
  initialized_(false),
  move_(false)
{
  atomic_init32(&jobs_pending_);
  atomic_init32(&jobs_failed_);
  atomic_init32(&death_sentences_executed_);
}


bool Spooler::Initialize() {
  LogCvmfs(kLogSpooler, kLogVerboseMsg, "Initializing Spooler backend");

  // initialize synchronisation for job queue (PushWorkers)
  if (pthread_mutex_init(&job_queue_mutex_, NULL) != 0)         return false;
  if (pthread_cond_init(&job_queue_cond_not_full_, NULL) != 0)  return false;
  if (pthread_cond_init(&job_queue_cond_not_empty_, NULL) != 0) return false;
  if (pthread_cond_init(&jobs_all_done_, NULL) != 0)            return false;

  // spawn the PushWorker objects in their own threads
  if (!SpawnPushWorkers()) {
    LogCvmfs(kLogSpooler, kLogWarning, "Failed to spawn concurrent push workers");
    return false;
  }

  initialized_ = true;
  return true;
}


Spooler::~Spooler() {
  LogCvmfs(kLogSpooler, kLogVerboseMsg, "Spooler backend terminates");

  // free PushWorker synchronisation primitives
  pthread_cond_destroy(&job_queue_cond_not_full_);
  pthread_cond_destroy(&job_queue_cond_not_empty_);
  pthread_cond_destroy(&jobs_all_done_);
  pthread_mutex_destroy(&job_queue_mutex_);
}


void Spooler::Copy(const std::string &local_path,
                   const std::string &remote_path) {
  LogCvmfs(kLogSpooler, kLogVerboseMsg,
           "Spooler received 'copy': source %s, dest %s move %d",
           local_path.c_str(), remote_path.c_str(), move_);

  Schedule(new StorageCopyJob(local_path, remote_path, move_, this));
}


void Spooler::Process(const std::string &local_path,
                      const std::string &remote_dir,
                      const std::string &file_suffix) {
  LogCvmfs(kLogSpooler, kLogVerboseMsg,
           "Spooler received 'process': source %s, dest %s, "
           "postfix %s, move %d", local_path.c_str(),
           remote_dir.c_str(), file_suffix.c_str(), move_);

  Schedule(new StorageCompressionJob(local_path, remote_dir, file_suffix, move_, this));
}


void Spooler::EndOfTransaction() {
  assert(!transaction_ends_);

  LogCvmfs(kLogSpooler, kLogVerboseMsg,
           "Spooler received 'end of transaction'");

  // Schedule a death sentence for every running worker thread.
  // Since we have a FIFO queue the death sentences will be at the end of the
  // line waiting for the threads to kill them.
  const int number_of_threads = GetNumberOfWorkers();
  for (int i = 0; i < number_of_threads; ++i) {
    Schedule(new DeathSentenceJob(this));
  }

  transaction_ends_ = true;
}


void Spooler::Schedule(Job *job) {
  LogCvmfs(kLogSpooler, kLogVerboseMsg, "scheduling new job into job queue: %s",
           job->name().c_str());

  // lock the job queue
  LockGuard<pthread_mutex_t> guard(job_queue_mutex_);

  // wait until there is space in the job queue
  while (job_queue_.size() >= (size_t)spooler_definition_.max_pending_jobs) {
    pthread_cond_wait(&job_queue_cond_not_full_, &job_queue_mutex_);
  }

  // put something into the job queue
  job_queue_.push(job);
  atomic_inc32(&jobs_pending_);

  // wake all waiting threads
  pthread_cond_broadcast(&job_queue_cond_not_empty_);
}


Job* Spooler::AcquireJob() {
  // lock the job queue
  LockGuard<pthread_mutex_t> guard(job_queue_mutex_);

  // wait until there is something to do
  while (job_queue_.empty()) {
    pthread_cond_wait(&job_queue_cond_not_empty_, &job_queue_mutex_);
  }

  // get the job and remove it from the queue
  Job* job = job_queue_.front();
  job_queue_.pop();

  // signal the Spooler that there is a fair amount of free space now
  static const size_t desired_free_slots =
    (size_t)spooler_definition_.max_pending_jobs / 2 + 1;
  if (job_queue_.size() < desired_free_slots) {
    pthread_cond_signal(&job_queue_cond_not_full_);
  }

  // return the acquired job
  LogCvmfs(kLogSpooler, kLogVerboseMsg, "acquired a job from the job queue: %s",
           job->name().c_str());
  return job;
}


void Spooler::WaitForUpload() const {
  // lock the job queue
  LockGuard<pthread_mutex_t> guard(job_queue_mutex_);

  LogCvmfs(kLogSpooler, kLogVerboseMsg, "Waiting for all jobs to be finished...");

  // wait until all pending jobs are processed
  while (atomic_read32(&jobs_pending_) > 0) {
    pthread_cond_wait(&jobs_all_done_, &job_queue_mutex_);
  }

  LogCvmfs(kLogSpooler, kLogVerboseMsg, "Jobs are done... go on");
}


void Spooler::JobFinishedCallback(Job* job) {
  // BEWARE!
  // This callback might be called from a different thread!

  // check if the finished job was successful
  if (job->IsSuccessful()) {
    LogCvmfs(kLogSpooler, kLogVerboseMsg, "Spooler Job '%s' succeeded.",
             job->name().c_str());
  } else {
    atomic_inc32(&jobs_failed_);
    LogCvmfs(kLogSpooler, kLogWarning, "Spooler Job '%s' failed.",
             job->name().c_str());
  }

  // invoke the external callback for this job
  InvokeExternalCallback(job);

  // check if we have killed all PushWorker threads
  if (job->IsDeathSentenceJob()) {
    atomic_inc32(&death_sentences_executed_);
    if (atomic_read32(&death_sentences_executed_) == GetNumberOfWorkers()) {
      TearDown();
    }
  }

  // remove the finished job from the pending 'list'
  delete job;
  atomic_dec32(&jobs_pending_);

  // Signal the Spooler that all jobs are done...
  if (atomic_read32(&jobs_pending_) == 0) {
    pthread_cond_signal(&jobs_all_done_);
  }
}


void Spooler::InvokeExternalCallback(Job* job) {
  // check if there is actually a callback to be invoked
  if (NULL == callback_)
    return;

  // call the callback for a finished compression job
  if (job->IsCompressionJob()) {
    StorageCompressionJob *compression_job =
      dynamic_cast<StorageCompressionJob*>(job);
    (*callback_)(compression_job->local_path(),
                 compression_job->return_code(),
                 compression_job->content_hash());
  } else

  // call the callback for a finished copy job
  if (job->IsCopyJob()) {
    StorageCopyJob *copy_job =
      dynamic_cast<StorageCopyJob*>(job);
    (*callback_)(copy_job->local_path(),
                 copy_job->return_code());
  }
}


void Spooler::SetCallback(SpoolerCallbackBase *callback_object) {
  assert (callback_ == NULL);
  callback_ = callback_object;
}


void Spooler::UnsetCallback() {
  delete callback_;
  callback_ = NULL;
}


// -----------------------------------------------------------------------------

bool LocalStat::Stat(const std::string &path) {
  return FileExists(base_path_ + "/" + path);
}

namespace upload {

  BackendStat *GetBackendStat(const std::string &spooler_definition) {
    std::vector<std::string> components = SplitString(spooler_definition, ',');
    std::vector<std::string> upstream = SplitString(components[0], ':');
    if ((upstream.size() != 2) || (upstream[0] != "local")) {
      PrintError("Invalid upstream");
      return NULL;
    }
    return new LocalStat(upstream[1]);
  }
  
}

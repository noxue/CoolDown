#include "job.h"
#include "local_sock_manager.h"
#include "download_task.h"
#include "client.h"
#include "client.pb.h"
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <Poco/Util/Application.h>
#include <Poco/Observer.h>
#include <Poco/Path.h>
#include <Poco/File.h>
//#include <Poco/Bugcheck.h>

using std::vector;
using std::min_element;
using Poco::Util::Application;
using Poco::Observer;
using Poco::Path;
using Poco::File;
using namespace ClientProto;

namespace CoolDown{
    namespace Client{

        Job::Job(const JobInfoPtr& info, LocalSockManager& m, Logger& logger)
        :app_( dynamic_cast<CoolClient&>( Application::instance() )),
        jobInfoPtr_(info),
        jobInfo_(*jobInfoPtr_), 
        sockManager_(m), 
        cs_(jobInfo_, sockManager_), 
        tp_(),
        tm_(tp_),
        logger_(logger){
            tm_.addObserver(
                    Observer<Job, TaskFinishedNotification>
                    (*this, &Job::onFinished)
                    );
            tm_.addObserver(
                    Observer<Job, TaskFailedNotification>
                    (*this, &Job::onFailed)
                    );
        }

        Job::~Job(){
            tm_.removeObserver(
                    Observer<Job, TaskFinishedNotification>
                    (*this, &Job::onFinished)
                    );
            tm_.removeObserver(
                    Observer<Job, TaskFailedNotification>
                    (*this, &Job::onFailed)
                    );
        }

        JobInfoPtr Job::MutableJobInfo(){
            return this->jobInfoPtr_;
        }
        const JobInfo& Job::JobInfo() const{
            return this->jobInfo_;
        }

        void Job::convert_bitmap_to_transport_format(const file_bitmap_ptr& bitmap, ClientProto::FileInfo* pInfo){
            using namespace std;
            poco_assert( pInfo != NULL );
            poco_assert( bitmap.isNull() == false );
            pInfo->set_filebitcount( bitmap->size() );
            to_block_range( *bitmap, google::protobuf::RepeatedFieldBackInserter(pInfo->mutable_filebit()) );
        }

        void Job::conver_transport_format_bitmap(const ClientProto::FileInfo& info, file_bitmap_ptr& bitmap){
            bitmap.assign( new file_bitmap_t( info.filebit().begin(), info.filebit().end() ) );
            bitmap->resize( info.filebitcount() );
        }
        
        void Job::onFinished(TaskFinishedNotification* pNf){
            DownloadTask* pTask = dynamic_cast<DownloadTask*>( pNf->task() );
            if( pTask == NULL ){
                poco_error(logger_, "cannot cast Task* to DownloadTask*, usually means fatal error.");
                return;
            }else if( pTask->reported() ){
                //this task has been reported by onFailed, do nothing;
            }else{
                cs_.report_success_chunk( pTask->chunk_pos(), pTask->fileid() );
            }
            //stuff all task must do
            pTask->set_reported();
            sockManager_.return_sock(pTask->clientid(), pTask->sock() );
            max_payload_cond_.signal();
            this->available_thread_cond_.signal();
        }

        void Job::onFailed(TaskFailedNotification* pNf){
            DownloadTask* pTask = dynamic_cast<DownloadTask*>( pNf->task() );
            if( pTask == NULL ){
                poco_error(logger_, "cannot cast Task* to DownloadTask*, usually means fatal error.");
                return;
            }
            poco_notice_f2(logger_,"task failed, name : %s, reason : %s", pTask->name(), pNf->reason().displayText());
            cs_.report_failed_chunk( pTask->chunk_pos(), pTask->fileid() );
            pTask->set_reported();
        }

        void Job::run(){
            poco_debug(logger_, "Job start running!");
            vector<string> fileidlist( cs_.fileidlist() );
            vector<string>::iterator iter = fileidlist.begin();
            retcode_t ret = ERROR_OK;
            while( iter != fileidlist.end() ){
                string fileid(*iter);

                ret = this->request_clients(fileid);
                poco_debug_f2(logger_, "request clients for fileid : %s, return code : %d", fileid, (int)ret);
                if( ret == ERROR_FILE_NO_OWNER_ONLINE ){
                    poco_debug_f1(logger_, "no online client has file : %s", fileid);
                }else
                    if( ret != ERROR_OK ){
                        poco_warning_f1(logger_, "Cannot request_clients of fileid : %s", fileid);
                    }else{
                        poco_debug_f1(logger_, "request_clients succeed, fileid : %s", fileid);
                        
                        FileOwnerInfoPtrList& infoList = jobInfo_.ownerInfoMap[fileid];
                        FileOwnerInfoPtrList::iterator infoIter = infoList.begin();
                        poco_debug(logger_, "Before walk FileOwnerInfoPtrList to shake_hand");
                        while( infoIter != infoList.end() ){
                            if( !sockManager_.is_connected((*infoIter)->clientid) ){
                                retcode_t conn_ret = sockManager_.connect_client(
                                        (*infoIter)->clientid, (*infoIter)->ip, (*infoIter)->message_port);
                                poco_debug_f1(logger_, "connect_client return %d.", (int)conn_ret);

                                if( conn_ret != ERROR_OK ){
                                    poco_warning_f3(logger_, "Cannot connect client, id : %s, ip :%s, port : %d",
                                            (*infoIter)->clientid, (*infoIter)->ip, (*infoIter)->message_port);
                                }
                            }else{
                                poco_debug_f1(logger_, "already connect to %s, no need to connect again", (*infoIter)->clientid);
                            }

                            retcode_t shake_hand_ret = this->shake_hand(fileid, (*infoIter)->clientid);
                            if( shake_hand_ret != ERROR_OK ){
                                poco_warning_f2(logger_, "Cannot shake hand with clientid : %s, fileid : %s",
                                        (*infoIter)->clientid, fileid);
                            }else{
                                poco_debug_f2(logger_, "shake hand with client succeed, clientid : %s, fileid : %s",
                                        (*infoIter)->clientid, fileid);
                            }
                            ++infoIter;
                        }
                    }
                ++iter;
            }

            poco_debug(logger_, "Before init_queue.");
            cs_.init_queue();
            poco_debug(logger_, "After init_queue.");
            while(1){
                const static int WAIT_TIMEOUT = 1000;
                poco_debug(logger_, "enter Job::run() while(1)");
                //see if the Job(upload&download) has been shutdown
                if( jobInfo_.downloadInfo.is_finished ){
                    break;
                }
                //File file(jobInfo_.localFileInfo.local_file)
                //see if the download has been paused
                if( jobInfo_.downloadInfo.is_download_paused ){
                    poco_debug(logger_, "download paused, going to wait the download_pause_cond.");
                    jobInfo_.downloadInfo.download_pause_cond.wait(jobInfo_.downloadInfo.download_pause_mutex, WAIT_TIMEOUT);
                }else{
                    ChunkInfoPtr chunk_info = cs_.get_chunk();
                    if( chunk_info.isNull() ){
                        poco_debug(logger_, "All chunk have been processed. leave the while(1) loop");
                        break;
                    }else{
                        poco_debug_f2(logger_, "start downloading file '%s', chunk_pos : %d", chunk_info->fileid, chunk_info->chunk_num);
                    }
                    vector<double> payloads;
                    int client_count = chunk_info->clientLists.size();
                    if( client_count == 0 ){
                        poco_notice(logger_, "Current no client has this chunk.");
                        break;
                    }

                    poco_debug_f1(logger_, "Download chunk from %d clients", client_count);
                    payloads.reserve( client_count );

                    for(int i = 0; i != chunk_info->clientLists.size(); ++i){
                        FileOwnerInfoPtr ownerInfo = chunk_info->clientLists[i];
                        double payload_percentage = sockManager_.get_payload_percentage(ownerInfo->clientid);
                        poco_debug_f2(logger_, "client '%s', payload : %f", ownerInfo->clientid, payload_percentage);
                        payloads.push_back( payload_percentage );
                    }

                    vector<double>::iterator iter = min_element(payloads.begin(), payloads.end());
                    int index = iter - payloads.begin();
                    double payload_percentage = *iter;
                    poco_debug_f2(logger_, "Choose the index %d file owner, payload : %f", index, payload_percentage);

                    //see if the minimal payload is 100%
                    if( 1 - payload_percentage < 1e-6 ){
                        poco_debug(logger_, "Even the lowest payload client is 100% payload.");
                        try{
                            max_payload_cond_.wait(max_payload_mutex_, WAIT_TIMEOUT);
                            poco_notice(logger_, "wake up from max_payload_cond_");
                        }catch(Poco::TimeoutException& e){
                            poco_notice(logger_, "Wait max_payload_cond_ timeout!");
                        }
                        continue;
                    }else{
                        string clientid( chunk_info->clientLists[index]->clientid );
                        LocalSockManager::SockPtr sock = sockManager_.get_idle_client_sock(clientid);
                        //see if some error happend in get_idle_client_sock
                        if( sock.isNull() ){
                            poco_warning_f1(logger_, "Unexpected null SockPtr return by sockManager_.get_idle_client_sock, client id :%s"                                            ,clientid);
                            continue;
                        }else{
                            poco_debug(logger_, "Get peer idle socket succeed.");
                            int chunk_pos = chunk_info->chunk_num;
                            string fileid( chunk_info->fileid );
                            TorrentFileInfoPtr fileInfo = jobInfo_.torrentInfo.get_file(fileid);

                            if( false == jobInfo_.localFileInfo.has_file(fileid) ){
                                retcode_t create_file_ret = jobInfo_.localFileInfo.add_file( fileid, 
                                        fileInfo->relative_path(), fileInfo->filename(), fileInfo->size());
                                if( create_file_ret != ERROR_OK ){
                                    poco_warning_f2(logger_, "Cannot add file, file id '%s', relative path : '%s'",
                                            fileid, fileInfo->relative_path() );
                                    poco_assert( create_file_ret != ERROR_OK );
                                }else{
                                    poco_debug_f1(logger_, "add file '%s' to localFileInfo succeed!", fileid);
                                }
                            }else{
                                poco_debug_f1(logger_, "localFileInfo already contains file '%s'", fileid);
                            }

                            FilePtr file = jobInfo_.localFileInfo.get_file(fileid);
                            poco_trace(logger_, "Before start new download task.");
                            try{
                                poco_assert( fileInfo.isNull() == false );
                                poco_assert( sock.isNull() == false );
                                poco_assert( file.isNull() == false );
                                
                                poco_debug_f1(logger_, "available thread : %d", tp_.available() );
                                while( tp_.available() == 0 ){
                                    this->available_thread_cond_.wait( this->available_thread_mutex_ );
                                }
                                tm_.start( new DownloadTask(
                                            *fileInfo, 
                                            jobInfo_.downloadInfo, 
                                            jobInfo_.clientid(), 
                                            sock, 
                                            chunk_pos, 
                                            *file
                                            ) 
                                        );
                            }catch(Poco::Exception& e){
                                poco_warning_f1(logger_, "Got exception while start DownloadTask, %s", e.displayText() );
                            }
                        }
                    }
                }
            }
            tm_.joinAll();
            poco_debug(logger_, "Job finished!");
        }

        retcode_t Job::request_clients(const string& fileid){
            string tracker_address(jobInfo_.torrentInfo.tracker_address());
            poco_debug_f1(logger_, "going to request clients from %s", tracker_address);
            poco_assert( jobInfo_.downloadInfo.percentage_map.find(fileid) != jobInfo_.downloadInfo.percentage_map.end() );
            poco_debug_f2(logger_, "assert passed at file : %s, line : %d", string(__FILE__), __LINE__ - 1);
            int percentage = jobInfo_.downloadInfo.percentage_map[fileid];
            int needCount = 20;
            CoolClient::ClientIdCollection clientidList;
            JobInfo::owner_info_map_t& infoMap = jobInfo_.ownerInfoMap;
            JobInfo::owner_info_map_t::iterator iter = infoMap.find(fileid);
            if( iter != infoMap.end() ){
                FileOwnerInfoPtrList& infoPtrList = iter->second;
                for(int i = 0; i != infoPtrList.size(); ++i){
                    clientidList.push_back(infoPtrList[i]->clientid);
                }
            }else{
                infoMap[fileid] = FileOwnerInfoPtrList();
            }

            FileOwnerInfoPtrList res;
            retcode_t ret = app_.request_clients(tracker_address, fileid, percentage, 
                                          needCount, clientidList, &res);
            if( ret != ERROR_OK ){
                poco_warning_f1(logger_, "app_.request_clients return %d", (int)ret);
            }else{
                poco_debug_f2(logger_, "request clients return %d clients of fileid : %s", (int)res.size(), fileid);
                if( res.size() == 0 ){
                    return ERROR_FILE_NO_OWNER_ONLINE;
                }
                //remove all clients we already have and use the new client list
                infoMap[fileid] = res;
            }
            return ret;
        }

        retcode_t Job::shake_hand(const string& fileid, const string& clientid){
            ShakeHand self;
            //fill our info of this file
            self.set_clientid( app_.clientid() );
            FileInfo* pInfo = self.mutable_info();
            pInfo->set_fileid(fileid);
            pInfo->set_hasfile(1);
            poco_assert( jobInfo_.downloadInfo.percentage_map.find(fileid) != jobInfo_.downloadInfo.percentage_map.end() );
            poco_debug_f2(logger_, "assert passed at file : %s, line : %d", string(__FILE__), __LINE__ - 1);
            pInfo->set_percentage(jobInfo_.downloadInfo.percentage_map[fileid]);
            Job::convert_bitmap_to_transport_format(jobInfo_.downloadInfo.bitmap_map[fileid], pInfo);

            ShakeHand peer;
            //only fill the clientid as we only know
            peer.set_clientid( clientid );
            poco_trace_f1(logger_, "before shake_hand with client '%s'", clientid);
            retcode_t ret = app_.shake_hand(self, peer);
            if( ret != ERROR_OK ){
                poco_warning_f2(logger_, "shake hand with peer error! fileid : %s, clientid : %s", fileid, clientid);
                return ret;
            }
            poco_debug_f2(logger_, "shake hand with peer succeed! fileid : %s, clientid : %s", fileid, clientid);
            if( peer.info().hasfile() == 0 ){
                poco_information_f2(logger_, "remote peer '%s' has no file '%s'.", clientid, fileid);
                return ERROR_PEER_FILE_NOT_FOUND;
            }

            //find the ownerinfolist of this file
            JobInfo::owner_info_map_t& infoMap = jobInfo_.ownerInfoMap;
            JobInfo::owner_info_map_t::iterator iter = infoMap.find(fileid);
            poco_assert( iter != infoMap.end() );
            poco_trace_f2(logger_, "assert passed at file : %s, line : %d", string(__FILE__), __LINE__ - 1);

            //since all clients are in the list, we just update the bitmap of peer client
            FileOwnerInfoPtrList& infoPtrList = iter->second;
            FileOwnerInfoPtrList::iterator infoIter = find_if( infoPtrList.begin(), infoPtrList.end(), 
                                                           FileOwnerInfoPtrSelector(clientid) );
            FileOwnerInfoPtr info;
            poco_assert( infoPtrList.end() != infoIter );
            poco_trace_f2(logger_, "assert passed at file : %s, line : %d", string(__FILE__), __LINE__ - 1);
            info = *infoIter;
            Job::conver_transport_format_bitmap(peer.info(), info->bitmap_ptr);
            poco_assert( peer.info().filebitcount() == info->bitmap_ptr->size() );
            poco_trace_f2(logger_, "assert passed at file : %s, line : %d", string(__FILE__), __LINE__ - 1);

            return ERROR_OK;
        }
    }
}

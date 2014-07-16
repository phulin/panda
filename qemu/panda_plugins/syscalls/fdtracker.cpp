/* PANDABEGINCOMMENT
 * 
 * Authors:
 *  Tim Leek               tleek@ll.mit.edu
 *  Ryan Whelan            rwhelan@ll.mit.edu
 *  Joshua Hodosh          josh.hodosh@ll.mit.edu
 *  Michael Zhivich        mzhivich@ll.mit.edu
 *  Brendan Dolan-Gavitt   brendandg@gatech.edu
 * 
 * This work is licensed under the terms of the GNU GPL, version 2. 
 * See the COPYING file in the top-level directory. 
 * 
PANDAENDCOMMENT */


// This module tracks the file names associated with file descriptors.
// It currently DOES NOT handle links AT ALL
// It tracks open(), etc., so only knows the names used by those functions

#if defined(SYSCALLS_FDS_TRACK_LINKS)
#error "Hard and soft links are not supported"
#endif

#include <map>
#include <string>
#include <list>
#include "weak_callbacks.hpp"
#include "syscalls.hpp"
#include <iostream>

extern "C" {
#include <fcntl.h>
#include "panda_plugin.h"
}

const target_ulong NULL_FD = 0;

using namespace std;

static target_ulong calc_retaddr(CPUState* env, target_ulong pc){
#if defined(TARGET_ARM)
    // Normal syscalls: return addr is stored in LR
    return mask_retaddr_to_pc(env->regs[14]);

    // Fork, exec
    uint8_t offset = 0;
    if(env->thumb == 0){
        offset = 4;
    } else {
        offset = 2;
    }
    return pc + offset;
#elif defined(TARGET_I386)
    
#else
    
#endif
}

typedef map<int, string> fdmap;

map<target_ulong, fdmap> asid_to_fds;

#if defined(CONFIG_PANDA_VMI)
extern "C" {
#include "introspection/DroidScope/LinuxAPI.h"
// sched.h contains only preprocessor defines to constant literals
#include <linux/sched.h>
}

//#define TEST_FORK
#ifdef TEST_FORK
map<target_ulong, bool> tracked_forks;
#endif

// copy any descriptors from parent ASID to child ASID that aren't set in child
static void copy_fds(target_asid parent_asid, target_asid child_asid){
    for(auto parent_mapping : asid_to_fds[parent_asid]){
        auto child_it = asid_to_fds[child_asid].find(parent_mapping.first);
        if (child_it == asid_to_fds[child_asid].end()){
            asid_to_fds[child_asid][parent_mapping.first] = parent_mapping.second;
        }
    }
}

list<target_asid> outstanding_child_asids;
map<target_ulong, target_asid> outstanding_child_pids;


/* Deal with all scheduling cases:
 * - Parent returns first: PID of child is logged for copying
 * - Child returns first, not in VMI table yet: ASID is logged for copy at next chance
 * - Child returns first, in VMI table: copy will occur when parent returns :)
 * - Parent returns 
 * 
 * - parent runs first, child runs second but this callback runs 
 *      BEFORE the VMI can register the child process
 */
static int return_from_fork(CPUState *env){
    target_ulong child_pid = get_return_val(env);
    if(0 == child_pid){
        // This IS the child!
        assert("return_from_fork should only ever be called for the parent!");
        target_asid  asid;
        target_ulong pc;
        target_ulong cs_base;
        int flags;
        cpu_get_tb_cpu_state(env, &pc, &cs_base, &flags);
        asid = get_asid(env, pc);
        // See if the VMI can tell us our PID
        ProcessInfo* self_child = findProcessByPGD(asid);
        if(nullptr == self_child){
            // no, we can't look up our PID yet
            outstanding_child_asids.push_back(get_asid(env, pc));
        }else{
            auto it = outstanding_child_pids.find(self_child->pid);
            if (it == outstanding_child_pids.end()){
                outstanding_child_asids.push_back(get_asid(env, pc));
            }else{
                target_asid parent_asid = it->second;
                copy_fds(parent_asid, asid);
                outstanding_child_pids.erase(it);
            }
        }
        return 0;
    }

    // returned to the parent
    ProcessInfo* child = findProcessByPID(child_pid);
    if(nullptr == child){
        // child hasn't run yet!
        target_ulong pc;
        target_ulong cs_base;
        int flags;
        cpu_get_tb_cpu_state(env, &pc, &cs_base, &flags);
        // log that this ASID is the parent of the child's PID
        outstanding_child_pids[child_pid] = get_asid(env, pc);
#ifdef TEST_FORK
        tracked_forks[child_pid] = false;
#endif
        return 0;
    }
    //we're in the parent and the child has run
    target_ulong pc;
    target_ulong cs_base;
    int flags;
    cpu_get_tb_cpu_state(env, &pc, &cs_base, &flags);

    copy_fds(get_asid(env, pc), child->pgd);
    outstanding_child_asids.remove(child->pgd);
#ifdef TEST_FORK
    tracked_forks[child_pid] = true;
#endif
    return 0;
}

static void preExecForkCopier(CPUState* env, target_ulong pc){
#ifdef TEST_FORK
    for(auto fork : tracked_forks){
        cout << "Forked process " << fork.first << ": " << fork.second << endl;
    }
#endif
    //is this process in outstanding_child_pids?
    if (outstanding_child_pids.empty()) {
        return;
    }
    target_asid my_asid = get_asid(env, pc);
    ProcessInfo* my_proc = findProcessByPGD(my_asid);
    if(nullptr == my_proc){
        // VMI doen't know about me yet... weird
        return;
    }
    auto it = outstanding_child_pids.find(my_proc->pid);
    if (it == outstanding_child_pids.end()){
        return;
    }
    // this is a process we're looking for!
    copy_fds(it->second, my_asid);
    outstanding_child_pids.erase(it);
#ifdef TEST_FORK
    tracked_forks[my_proc->pid] = true;
#endif
}

//#define TEST_CLONE
#ifdef TEST_CLONE
map<target_ulong, bool> tracked_clones;
#endif


/* Clone is weird. We don't care about all of them.
   Instead of registering an AFTER_CLONE callback, we'll just
   use the plugin's internal callback mechanism so we can skip ones
   we don't want (which are distinguished by the arguments)*/

class CloneCallbackData : public CallbackData {   
};

list<target_asid> outstanding_clone_child_asids;
map<target_ulong, target_asid> outstanding_clone_child_pids;

static void clone_callback(CallbackData* opaque, CPUState* env, target_asid asid){
    CloneCallbackData* data = dynamic_cast<CloneCallbackData*>(opaque);
    if(!data){
        fprintf(stderr, "oops\n");
        return;
    }
    // return value is TID = PID of child
    target_ulong child_pid = get_return_val(env);
    if(0 == child_pid){
        // I am the child.
        // This should never happen
        cerr << "Called after-clone callback in child, not parent!" << endl;
    } else if (-1 == child_pid){
        // call failed
    } else {
        ProcessInfo* child = findProcessByPID(child_pid);
        if(nullptr == child){
            // child hasn't run yet!
            target_ulong pc;
            target_ulong cs_base;
            int flags;
            cpu_get_tb_cpu_state(env, &pc, &cs_base, &flags);

            // log that this ASID is the parent of the child's PID
            outstanding_clone_child_pids[child_pid] = asid;
#ifdef TEST_CLONE
            tracked_clones[child_pid] = false;
#endif
        } else {
            //we're in the parent and the child has run
            target_ulong pc;
            target_ulong cs_base;
            int flags;
            cpu_get_tb_cpu_state(env, &pc, &cs_base, &flags);
            // sanity check: make sure it's really a new process, not a thread
            if(child->pgd == asid){
                cerr << "Attempted to track a clone that was thread-like" << endl;
                return;
            }
            copy_fds(asid, child->pgd);
            outstanding_clone_child_asids.remove(child->pgd);
#ifdef TEST_CLONE
            tracked_clones[child_pid] = true;
#endif
        }
    }
}

// if flags includes CLONE_FILES then the parent and child will continue to share a single FD table
// if flags includes CLONE_THREAD, then we don't care about the call.
void call_clone_callback(CPUState* env,target_ulong pc,uint32_t clone_flags,uint32_t newsp,
                         target_ulong parent_tidptr,uint32_t tls_val,
                         target_ulong child_tidptr,target_ulong regs) {
    if (CLONE_THREAD & clone_flags){
        return;
    }
    if (CLONE_FILES & clone_flags){
        cerr << "ERROR ERROR UNIMPLEMENTED!" << endl;
    }
    CloneCallbackData *data = new CloneCallbackData;
    appendReturnPoint(ReturnPoint(calc_retaddr(env, pc), get_asid(env, pc), data, clone_callback));
}

static void preExecCloneCopier(CPUState* env, target_ulong pc){
#ifdef TEST_CLONE
    for(auto clone : tracked_clones){
        cout << "Cloned process " << clone.first << ": " << clone.second << endl;
    }
#endif
    //is this process in outstanding_child_pids?
    if (outstanding_clone_child_pids.empty()) {
        return;
    }
    target_asid my_asid = get_asid(env, pc);
    ProcessInfo* my_proc = findProcessByPGD(my_asid);
    if(nullptr == my_proc){
        // VMI doen't know about me yet... weird
        return;
    }
    auto it = outstanding_clone_child_pids.find(my_proc->pid);
    if (it == outstanding_clone_child_pids.end()){
        return;
    }
    // this is a process we're looking for!
    copy_fds(it->second, my_asid);
    outstanding_clone_child_pids.erase(it);
#ifdef TEST_CLONE
    tracked_clones[my_proc->pid] = true;
#endif
}

struct StaticBlock {
    StaticBlock(){
        registerExecPreCallback(preExecForkCopier);
        registerExecPreCallback(preExecCloneCopier);
        panda_cb pcb;

        pcb.return_from_fork = return_from_fork;
        panda_register_callback(syscalls_plugin_self, PANDA_CB_VMI_AFTER_FORK, pcb);
    }
};
static StaticBlock staticBlock;

#endif

class OpenCallbackData : public CallbackData {
public:
    string path;
    target_ulong base_fd;
};

class DupCallbackData: public CallbackData {
public:
    target_ulong old_fd;
    target_ulong new_fd;
};


static void open_callback(CallbackData* opaque, CPUState* env, target_asid asid){
    OpenCallbackData* data = dynamic_cast<OpenCallbackData*>(opaque);
    if (-1 == get_return_val(env)){
        return;
    }
    if(!data){
        fprintf(stderr, "oops\n");
        return;
    }
    string dirname = "";
    auto& mymap = asid_to_fds[asid];
    
    if(NULL_FD != data->base_fd){
        dirname += mymap[data->base_fd];
    }
    dirname += "/" + data->path;
    mymap[get_return_val(env)] = dirname;
}

//mkdirs
void call_sys_mkdirat_callback(CPUState* env,target_ulong pc,uint32_t dfd,std::string pathname,uint32_t mode) { 
    //mkdirat does not return an FD
    /*OpenCallbackData* data = new OpenCallbackData;
    data->path = pathname;
    data->base_fd = dfd;
    appendReturnPoint(ReturnPoint(calc_retaddr(env, pc), get_asid(env, pc), data, open_callback));*/
}

void call_sys_mkdir_callback(CPUState* env,target_ulong pc,std::string pathname,uint32_t mode) { 
    // mkdir does not return an FD
    /*OpenCallbackData* data = new OpenCallbackData;
    data->path = pathname;
    data->base_fd = NULL_FD;
    appendReturnPoint(ReturnPoint(calc_retaddr(env, pc), get_asid(env, pc), data, open_callback));*/
}
//opens

void call_sys_open_callback(CPUState *env, target_ulong pc, std::string filename,uint32_t flags,uint32_t mode){
    OpenCallbackData* data = new OpenCallbackData;
    data->path = filename;
    data->base_fd = NULL_FD;
    appendReturnPoint(ReturnPoint(calc_retaddr(env, pc), get_asid(env, pc), data, open_callback));
}

void call_sys_openat_callback(CPUState* env,target_ulong pc,uint32_t dfd,std::string filename,uint32_t flags,uint32_t mode){
    OpenCallbackData* data = new OpenCallbackData;
    data->path = filename;
    data->base_fd = dfd;
    if (dfd == AT_FDCWD)
        data->base_fd = NULL_FD;
    appendReturnPoint(ReturnPoint(calc_retaddr(env, pc), get_asid(env, pc), data, open_callback));
}

static void dup_callback(CallbackData* opaque, CPUState* env, target_asid asid){
    DupCallbackData* data = dynamic_cast<DupCallbackData*>(opaque);
    if(!data){
        fprintf(stderr, "oops\n");
        return;
    }
    target_ulong new_fd;
    if(data->new_fd != NULL_FD){
        new_fd = data->new_fd;
    }else{
        new_fd = get_return_val(env);
    }
    asid_to_fds[asid][new_fd] = asid_to_fds[asid][data->old_fd];
}

// dups
void call_sys_dup_callback(CPUState* env,target_ulong pc,uint32_t fildes) {
    DupCallbackData* data = new DupCallbackData;
    data->old_fd = fildes;
    data->new_fd = NULL_FD;
    appendReturnPoint(ReturnPoint(calc_retaddr(env, pc), get_asid(env, pc), data, dup_callback));
    
}
void call_sys_dup2_callback(CPUState* env,target_ulong pc,uint32_t oldfd,uint32_t newfd) { 
    target_asid asid = get_asid(env, pc);
    asid_to_fds[asid][newfd] = asid_to_fds[asid][oldfd];
    return;
    
    DupCallbackData* data = new DupCallbackData;
    data->old_fd = oldfd;
    data->new_fd = newfd;
    appendReturnPoint(ReturnPoint(calc_retaddr(env, pc), get_asid(env, pc), data, dup_callback));
    
}
void call_sys_dup3_callback(CPUState* env,target_ulong pc,uint32_t oldfd,uint32_t newfd,uint32_t flags) {
    target_asid asid = get_asid(env, pc);
    asid_to_fds[asid][newfd] = asid_to_fds[asid][oldfd];
    return;
    
    DupCallbackData* data = new DupCallbackData;
    data->old_fd = oldfd;
    data->new_fd = newfd;
    appendReturnPoint(ReturnPoint(calc_retaddr(env, pc), get_asid(env, pc), data, dup_callback));
    
}

// close
void call_sys_close_callback(CPUState* env,target_ulong pc,uint32_t fd) { }

void call_sys_readahead_callback(CPUState* env,target_ulong pc,uint32_t fd,uint64_t offset,uint32_t count) { }

void call_sys_read_callback(CPUState* env,target_ulong pc,uint32_t fd,target_ulong buf,uint32_t count) {
    target_asid asid = get_asid(env, pc);
    cout << "Reading from " << asid_to_fds[asid][fd] << endl;
}
void call_sys_readv_callback(CPUState* env,target_ulong pc,uint32_t fd,target_ulong vec,uint32_t vlen) { 
    target_asid asid = get_asid(env, pc);
    cout << "Reading v from " << asid_to_fds[asid][fd] << endl;
}
void call_sys_pread64_callback(CPUState* env,target_ulong pc,uint32_t fd,target_ulong buf,uint32_t count,uint64_t pos) {
        target_asid asid = get_asid(env, pc);
        cout << "Reading p64 from " << asid_to_fds[asid][fd] << endl;
}
void call_sys_write_callback(CPUState* env,target_ulong pc,uint32_t fd,target_ulong buf,uint32_t count) {
    target_asid asid = get_asid(env, pc);
    cout << "Writing to " << asid_to_fds[asid][fd] << endl;
}
void call_sys_pwrite64_callback(CPUState* env,target_ulong pc,uint32_t fd,target_ulong buf,uint32_t count,uint64_t pos) { 
    target_asid asid = get_asid(env, pc);
    cout << "Writing pv64 to " << asid_to_fds[asid][fd] << endl;
}
void call_sys_writev_callback(CPUState* env,target_ulong pc,uint32_t fd,target_ulong vec,uint32_t vlen) {
    target_asid asid = get_asid(env, pc);
    cout << "Writing v to " << asid_to_fds[asid][fd] << endl;
}

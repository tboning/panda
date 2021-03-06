#define __STDC_FORMAT_MACROS

#include "../taint2/taint2.h"

extern "C" {

#include "rr_log.h"    
#include "qemu-common.h"
#include "cpu.h"
#include "panda_plugin.h"
#include "pandalog.h"
#include "panda_common.h"
#include "../syscalls2/gen_syscalls_ext_typedefs_linux_x86.h"
#include "../taint/taint_ext.h"
#include "../taint2/taint2_ext.h"
#include "panda_plugin_plugin.h" 
    
    bool init_plugin(void *);
    void uninit_plugin(void *);
    
    int get_loglevel() ;
    void set_loglevel(int new_loglevel);

}

const char *taint_filename = 0;
bool positional_labels = true;
bool use_taint2 = true;
bool no_taint = true;

#define MAX_FILENAME 256
bool saw_open = false;
uint32_t the_asid;
uint32_t the_fd;


#ifdef TARGET_I386
// 5 long sys_open(const char __user *filename,int flags, int mode);
// typedef void (*on_sys_open_enter_t)(CPUState* env,target_ulong pc,target_ulong filename,int32_t flags,int32_t mode);
void open_enter(CPUState* env,target_ulong pc,target_ulong filename,int32_t flags,int32_t mode) {
    uint32_t i;
    char the_filename[MAX_FILENAME];
    the_filename[0] = 0;
    for (i=0; i<MAX_FILENAME; i++) {
        uint8_t c;
        panda_virtual_memory_rw(env, filename+i, &c, 1, 0);
        the_filename[i] = c;
        if (c==0) {            
            break;
        }
    }
    the_filename[MAX_FILENAME-1] = 0;
    if (i != 0 ) {
        printf ("saw open of [%s]\n", the_filename);
    }
    if (i == strlen(taint_filename)) {
        if (strncmp(the_filename, taint_filename, strlen(the_filename)) == 0) {
            saw_open = true;
            printf ("saw open of file we want to taint: [%s]\n", taint_filename);
            the_asid = panda_current_asid(env);
        }
    }
}


void open_return(CPUState* env,target_ulong pc,target_ulong filename,int32_t flags,int32_t mode) {
    //    printf ("returning from open\n");
    if (saw_open && the_asid == panda_current_asid(env)) {
        saw_open = false;
        // get return value, which is the file descriptor for this file
        the_asid = panda_current_asid(env);
        the_fd = EAX;
        printf ("saw return from open of [%s]: asid=0x%x  fd=%d\n", taint_filename, the_asid, the_fd);
    }
            
}
   

uint32_t the_buf;
uint32_t the_count;
bool saw_read = false;

void read_enter(CPUState* env,target_ulong pc,uint32_t fd,target_ulong buf,uint32_t count) { 
    if (panda_current_asid(env) == the_asid && fd == the_fd) {
        the_buf = buf;
        the_count = count;
        printf ("saw read of [%s] %d bytes\n", taint_filename, count);
        saw_read = true;
    }
}


uint32_t taint_label_number_start = 0;

// 3 long sys_read(unsigned int fd, char __user *buf, size_t count);
// typedef void (*on_sys_read_return_t)(CPUState* env,target_ulong pc,uint32_t fd,target_ulong buf,uint32_t count);
void read_return(CPUState* env,target_ulong pc,uint32_t fd,target_ulong buf,uint32_t count) { 

    
    if (saw_read && panda_current_asid(env) == the_asid) {
        count = EAX;
        printf ("count=%d\n", count);
        
        printf ("returning from read of [%s] count=%d\n", taint_filename, count);    
        printf ("*** applying %s taint labels %d..%d to buffer\n",
                positional_labels ? "positional" : "uniform",
                taint_label_number_start, taint_label_number_start + count - 1);
        for (uint32_t i=0; i<count; i++ ) {
            target_phys_addr_t pa = panda_virt_to_phys(env, the_buf+i);
            if (pandalog) {
                Panda__LogEntry ple = PANDA__LOG_ENTRY__INIT;
                ple.has_taint_label_virtual_addr = 1;
                ple.has_taint_label_physical_addr = 1;
                ple.has_taint_label_number = 1;
                ple.taint_label_virtual_addr = the_buf + i;
                ple.taint_label_physical_addr = pa;
                if (positional_labels) {
                    ple.taint_label_number = taint_label_number_start + i;
                }
                else {
                    ple.taint_label_number = 1;
                }
                pandalog_write_entry(&ple);           
            }
            if (!no_taint) {
                if (positional_labels) {
                    if (use_taint2) 
                        taint2_label_ram(pa, taint_label_number_start + i);
                    else 
                        taint_label_ram(pa, i);
                }
                else {
                    if (use_taint2) 
                        taint2_label_ram(pa, 1);
                    else 
                        taint_label_ram(pa, 1);
                }
            }
        }
        taint_label_number_start += count;
        printf (" ... done applying labels\n");
        saw_read = false;
    }
}
#endif

bool init_plugin(void *self) {

    printf("Initializing plugin file_taint\n");

    panda_arg_list *args;
    args = panda_get_args("file_taint");
    taint_filename = panda_parse_string(args, "filename", "abc123");
    positional_labels = panda_parse_bool(args, "pos");
    use_taint2 = !panda_parse_bool(args, "taint1");
    // used to just find the names of files that get 
    no_taint = panda_parse_bool(args, "notaint");

    printf ("taint_filename = [%s]\n", taint_filename);
    printf ("positional_labels = %d\n", positional_labels);


    panda_require("syscalls2");

    // this sets up the taint api fn ptrs so we have access
    if (!no_taint) {
        if (use_taint2) {
            panda_require("taint2");
            assert(init_taint2_api());
            taint2_enable_taint();
        } else {
            panda_require("taint");
            assert(init_taint_api());
            taint_enable_taint();
        }
    }
    
#if defined(TARGET_I386)
            
    PPP_REG_CB("syscalls2", on_sys_open_enter, open_enter);
    PPP_REG_CB("syscalls2", on_sys_open_return, open_return);
    
    PPP_REG_CB("syscalls2", on_sys_read_enter, read_enter);
    PPP_REG_CB("syscalls2", on_sys_read_return, read_return);
    
#endif
    return true;
}


void uninit_plugin(void *self) {
}


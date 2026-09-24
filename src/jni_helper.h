#ifndef MP_JNI_HELPER_H
#define MP_JNI_HELPER_H
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <link.h>
#include <elf.h>
static void* g_libart_base = 0;
static char g_libart_path[256];
static int phdr_cb(struct dl_phdr_info* info, size_t size, void* data){
    (void)size; (void)data;
    if(!info->dlpi_name || !info->dlpi_name[0]) return 0;
    const char* n = info->dlpi_name;
    if(!g_libart_base && (strstr(n,"libart.so")||strstr(n,"libnativehelper.so"))){
        g_libart_base = (void*)info->dlpi_addr;
        strncpy(g_libart_path, n, sizeof(g_libart_path)-1);
    }
    return 0;
}
static void* find_sym_loaded(const char* want){
    if(!g_libart_base || !g_libart_path[0]) return 0;
    int fd = open(g_libart_path, O_RDONLY);
    if(fd < 0) return 0;
    Elf64_Ehdr eh;
    if(read(fd,&eh,sizeof(eh))!=sizeof(eh)){ close(fd); return 0; }
    if(memcmp(eh.e_ident,ELFMAG,4)!=0){ close(fd); return 0; }
    Elf64_Phdr ph;
    Elf64_Off dyn_off=0; Elf64_Xword dyn_sz=0;
    for(int i=0;i<eh.e_phnum;i++){
        if(pread(fd,&ph,sizeof(ph),eh.e_phoff+i*sizeof(ph))!=sizeof(ph)) break;
        if(ph.p_type==PT_DYNAMIC){ dyn_off=ph.p_offset; dyn_sz=ph.p_filesz; break; }
    }
    if(!dyn_off){ close(fd); return 0; }
    Elf64_Dyn dyn; Elf64_Addr symtab=0, strtab=0;
    for(Elf64_Xword off=0; off+sizeof(dyn)<=dyn_sz; off+=sizeof(dyn)){
        if(pread(fd,&dyn,sizeof(dyn),dyn_off+off)!=sizeof(dyn)) break;
        if(dyn.d_tag==DT_SYMTAB) symtab=dyn.d_un.d_ptr;
        else if(dyn.d_tag==DT_STRTAB) strtab=dyn.d_un.d_ptr;
    }
    if(!symtab||!strtab){ close(fd); return 0; }
    Elf64_Off sym_foff=0, str_foff=0;
    for(int i=0;i<eh.e_phnum;i++){
        if(pread(fd,&ph,sizeof(ph),eh.e_phoff+i*sizeof(ph))!=sizeof(ph)) break;
        if(ph.p_type!=PT_LOAD) continue;
        if(symtab>=ph.p_vaddr && symtab<ph.p_vaddr+ph.p_filesz) sym_foff=ph.p_offset+(symtab-ph.p_vaddr);
        if(strtab>=ph.p_vaddr && strtab<ph.p_vaddr+ph.p_filesz) str_foff=ph.p_offset+(strtab-ph.p_vaddr);
    }
    if(!sym_foff||!str_foff){ close(fd); return 0; }
    Elf64_Sym sym; char nm[256];
    for(int k=0;k<200000;k++){
        if(pread(fd,&sym,sizeof(sym),sym_foff+k*sizeof(sym))!=sizeof(sym)) break;
        if(sym.st_name==0) continue;
        if(pread(fd,nm,sizeof(nm)-1,str_foff+sym.st_name)<=0) break;
        nm[sizeof(nm)-1]=0;
        if(strcmp(nm,want)==0){ close(fd); return (void*)((char*)g_libart_base+sym.st_value); }
    }
    close(fd);
    return 0;
}
#endif

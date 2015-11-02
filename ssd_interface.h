/* 
 *
 * Description: This is a header file for ssd_interface.c.
 *
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "fast.h"
#include "pagemap.h"
#include "flash.h"
#include "type.h"
#include "disksim_global.h"

#define READ_DELAY        0.2
#define WRITE_DELAY       2
#define ERASE_DELAY       5 
#define GC_READ_DELAY  READ_DELAY    // gc read_delay = read delay    
#define GC_WRITE_DELAY WRITE_DELAY   // gc write_delay = write delay

#define PPC_GC_INVLD 0
#define PPC_GC_VLD 1
#define NORMAL_LRU 2
#define NORMAL_LEN 3
#define NORMAL_CNT 4
#define NORMAL_LRU_CNT 5

#define GETINDEX 0

#define INDEX_LPN 0
#define INDEX_PARITY_LPN 1

#define FREE_BLK_LOWLEVEL   3
#define FLT_BUF_INFO_SIZE   2048
#define FTL_BUF_FREE        0
#define FTL_BUF_WAIT_DATA   1
#define FTL_BUF_DATA_ARRIVE 2
#define FTL_BUF_CPL         3
#define FTL_BUF_TX_CPL      4
#define FTL_BLK_OVERHEAD    0.013

#define FTLNV_STATIC_PC     (0.00005*3.3)//unit:w
#define FTLDR_STATIC_PC     (0.0000198779*4)//8MB
#define FTLFLASH_STATIC_PC  (0.00005*3.3)

#define FTLNV_RD_PC         (0.05*3.3)
#define FTLNV_WR_PC         (0.05*3.3)
#define FTLDR_DYN_PC        (0.0347223/1000000)//8MB(mJ)
#define FTLFLASH_PROG_PC    (0.05*3.3)
#define FTLFLASH_READ_PC    (0.05*3.3)
#define FTLFLASH_ERASE_PC   (0.05*3.3) 

#define FTLNV_RD_ACCTIME         0.00005
#define FTLNV_WR_ACCTIME         0.000150
#define FTLDR_DYN_ACCTIME        (0.000003*PAGE_SIZE_B)
#define FTLFLASH_PROG_ACCTIME    WRITE_DELAY
#define FTLFLASH_READ_ACCTIME    READ_DELAY
#define FTLFLASH_ERASE_ACCTIME   ERASE_DELAY

#define FTLLDR_REF_INTERVAL      64000.0

#define FTL_WAY_FREE        0
#define FTL_WAIT_DATA       1

#define OOB_READ_DELAY    0.0
#define OOB_WRITE_DELAY   0.0

struct ftl_operation * ftl_op;

#define PAGE_READ     0
#define PAGE_WRITE    1
#define OOB_READ      2
#define OOB_WRITE     3
#define BLOCK_ERASE   4
#define GC_PAGE_READ  5
#define GC_PAGE_WRITE 6

void reset_flash_stat();
double calculate_delay_flash();
void initFlash(char **argv);
void endFlash();
void printWearout();
void send_flash_request(int start_blk_no, int block_cnt, int operation, int mapdir_flag,unsigned int ftlcenum);
void find_real_max();
void find_real_min();
int find_min_ghost_entry();
void synchronize_disk_flash();
void find_min_cache();
double callFsim(unsigned int secno, int scount, int operation,unsigned int ftlcenum);
int FtlChkIobfnum( int bcount); 
int write_count;
int read_count;

int flash_read_num;
int flash_write_num;
int flash_gc_read_num;
int flash_gc_write_num;
int flash_erase_num;
int flash_oob_read_num;
int flash_oob_write_num;

int map_flash_read_num;
int map_flash_write_num;
int map_flash_gc_read_num;
int map_flash_gc_write_num;
int map_flash_erase_num;
int map_flash_oob_read_num;
int map_flash_oob_write_num;

int ftl_type;

extern int total_util_sect_num; 
extern int total_extra_sect_num;

int global_total_blk_num;

int warm_done; 

int total_er_cnt;
int flag_er_cnt;
int block_er_flag[20000];
int block_dead_flag[20000];
int wear_level_flag[20000];
int unique_blk_num; 
int unique_log_blk_num;
int last_unique_log_blk;

int total_extr_blk_num;
int total_init_blk_num;  

extern unsigned int visible_blkno;
extern unsigned int parity_blkno;
extern int page_per_strip;
extern int EVICT_THRES;

//define structure for io buffer queue on ftl simulator
typedef struct FltIobInfo {
   double    arrivetime;								//io command arrive time
   double    starttime; 								//write: data are available; read: data are in buffer or after 
   double    availtime; 								//when the buffer is free
   double    bufcpltime;								//write: data are available; read: data are in buffer
   int       state;
   struct    FltIobInfo *next;
   struct    FltIobInfo *prev;
   int       bcount;
   int       devno;
   unsigned int blkno;
   int       flags;
   int       curr_flag;									//for bufhit
} fltiob;

typedef struct FtlWaySubqueue {
   double  CplTime;
   double  CurrArriveTime;
   int     state;
   double  CurrCpltransTime;
} ftlway;

typedef struct FtlChnlSubqueue {
   int     MaxWayNum;
   double  WaysTransCplTime;
   ftlway  *wayinfo[16];								//the maximum way number is limited to 16
} ftlsub;

typedef struct FtlIobufQueue {
   int		        TotalBufSize;
   int            MaxReqBufSize;
   int		        AvailBufSize;
   int            UsedBufSize;
   ftlsub         *subqueue[64];				//the maximum channel number is limited to 64
   fltiob         *AvailIob_head;
   fltiob         *AvailIob_tail;   
   fltiob         *UsedIob_head;
   fltiob         *UsedIob_tail;
   fltiob         *CurrWaitQueue[262144];//max. 128B
   int            CwqPointer;
   int            OutstandingIobNum; 
   int            MaxChnlNum;     
} ftlqueue;

typedef struct FtlStatInfo {
   long int		    TotalLpNum;
   long int       TotalHitNum;
   double         Total_NVstatic_energy;	//unit: mA
   double         Total_NVaccess_energy;
   double         Total_Flashstatic_energy;
   double         Total_Flashaccess_energy;
   double         Total_Dramstatic_energy;
   double         Total_Dramaccess_energy; 
   double         Total_Dramrefresh_energy;   
   double         Total_Simtime;
   double         Total_IoReq;     
} ftlstat_t;

typedef struct  TableEntry {							//max. 32 pages/strip
   unsigned int   ParityLPN;							//if used for GC queue, ParityLPN = GCed LPN
   unsigned int   ModifiedLPN[32];				//if used for GC queue: 1: valid 0: invalid
   unsigned int   OldPBN[32];							//if used for GC queue, old pbn = GCed pbn
   int            TotalOldPBN;
   double         acctime;
   unsigned int   AccCnt;
   struct         TableEntry *next;
   struct         TableEntry *prev;
} entry_t;

typedef struct ParityCacheTable {
   entry_t	   *UsedHead;
   entry_t	   *UsedTail;
   entry_t     *AvailTail;
   entry_t     *AvailHead;   
   int          TotalNum;  
} pct_t;

typedef struct StripIndexEntry {
   unsigned int lpn[32];									//max strip:32
   int          vld[32];        					
   int          index_no;       					
   double       acctime;        					
   int          wr_strip_num;							//bit 0: for parity number; ~bit 1: for strip number
   struct       StripIndexEntry *next;
   struct       StripIndexEntry *prev;      
} sie_t;

typedef struct StripIndexTable {
   sie_t	   *UsedHead;
   sie_t	   *UsedTail;
   sie_t     *AvailTail;
   sie_t     *AvailHead;   
   int        TotalNum;  
} sit_t;

typedef struct RaidStripEntry {
   unsigned int lpn;
   int          index_no;									//bit 0: valid flag ~bit 1: index NO
   int          valid_flag;								//1: current entry is valid   
} rse_t;

pct_t *ppc_table;

unsigned int FtlGetCenum(unsigned int blkno);

double FtlGetIobOverhead(int valid_flag,unsigned int com_lpn);

double FtlUpdateIobState(ioreq_event *curr, double blktranstime);

fltiob * FtlChkBufHit(unsigned int blkno);

double FtlUpdateHitBufOverhead(unsigned int blkno, int flags, fltiob *iobuf);

double FtlGetNewIobOverhead(unsigned int blkno,int flags);

int FtlChkThreshold();

double FtlGetIobOverhead();

void FtlPrintStat(FILE *outFP);

void FtlStatUpdate(fltiob *iobuf, unsigned int cnt);

int FltGetOutstandingIobNum(unsigned int bcount);

void warmFlash(char *tname);

void flash_array_setup(char **argv);

double FtlCplReq(unsigned int blkno,int flags,fltiob *curriob);

unsigned int ChkOldPBN(unsigned int lba);

double CommitGCReadPage();

unsigned int ChkPPCOldPPN(unsigned int lpn);

void UpdateEntryInPPCTable(unsigned int lpn,unsigned int ppn,int flag);

int UpdateIndexEntry(sie_t *currindex,int strip_no, unsigned int lpn, int type);

double PlainRaidOpt(unsigned int rest_strip_start_no,int rest_page_num, int flag);

double FtlGetDDAIobOverhead(int valid_flag,unsigned int com_lpn);

double FtlGetHDAIobOverhead(int valid_flag,unsigned int com_lpn);

int ChkPPCTableHit(unsigned int lpn);

int ChkRaidBlk(unsigned int ppn);

int ChkStripeNum(unsigned int lbn, unsigned int num);

int ChkHitMemNum(unsigned int lpn, int num);

double CommitCurrParity(unsigned int parity_lpn, fltiob *curriob);
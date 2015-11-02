/* 
 * Contributors: Youngjae Kim (youkim@cse.psu.edu)
 *               Aayush Gupta (axg354@cse.psu.edu_
 *   
 * In case if you have any doubts or questions, kindly write to: youkim@cse.psu.edu 
 *
 * This source plays a role as bridiging disksim and flash simulator. 
 * 
 * Request processing flow: 
 *
 *  1. Request is sent to the simple flash device module. 
 *  2. This interface determines FTL type. Then, it sends the request
 *     to the lower layer according to the FTL type. 
 *  3. It returns total time taken for processing the request in the flash. 
 *
 */
#include "disksim_global.h"
#include "ssd_interface.h"
//#include "dftl.h"
#include <math.h> 

extern int merge_switch_num = 0;
extern int merge_partial_num = 0;
extern int merge_full_num = 0;
int old_merge_switch_num = 0;
int old_merge_partial_num = 0;
int old_merge_full_num= 0;
int old_flash_gc_read_num = 0;
int old_flash_erase_num = 0;
int req_count_num = 1;
int cache_hit, rqst_cnt;
int flag1 = 1;
int count = 0;
unsigned long int maxblk;
int FTL_BUF_NUM;
int FTL_BUF_THRU;  
int FTL_CHNL_NUM;    
int FTL_WAY_NUM;    
int FTL_CHNL_WIDTH; 
int FTL_WAY_WIDTH;   
int FTL_CE_NUM;
int cache_policy;
int page_per_strip;
unsigned int visible_blkno;
unsigned int parity_blkno;
int page_num_for_2nd_map_table;

#define MAP_REAL_MAX_ENTRIES 6552// real map table size in bytes
#define MAP_GHOST_MAX_ENTRIES 1640// ghost_num is no of entries chk if this is ok

#define CACHE_MAX_ENTRIES 300

extern ftlqueue *FtlBuf = NULL;
extern ftlstat_t *FtlStat = NULL;
extern pct_t *ppc_table = NULL;
extern pct_t *gc_page_table = NULL;
/***********************************************************************
  LINK TABLE for DDA
 ***********************************************************************/
extern sit_t *dda_strip_table = NULL;
//extern pct_t *dda_entry_link = NULL;
extern pct_t *rec_buf_table = NULL; 
int rec_buf_num;
int EVICT_THRES;
double raid_evict_overhead = 0;
unsigned int index_rd_cnt = 0;
unsigned int index_wr_cnt = 0;
unsigned int index_evict_rd_cnt = 0;
unsigned int index_evict_wr_cnt = 0;
unsigned int strip_rd_cnt = 0;
unsigned int strip_wr_cnt = 0;

/***********************************************************************
  Variables for statistics    
 ***********************************************************************/
unsigned int cnt_read = 0;
unsigned int cnt_write = 0;
unsigned int cnt_delete = 0;
unsigned int cnt_evict_from_flash = 0;
unsigned int cnt_evict_into_disk = 0;
unsigned int cnt_fetch_miss_from_disk = 0;
unsigned int cnt_fetch_miss_into_flash = 0;

double sum_of_queue_time = 0.0;
double sum_of_service_time = 0.0;
double sum_of_response_time = 0.0;
unsigned int total_num_of_req = 0;

/***********************************************************************
  Mapping table
 ***********************************************************************/
int real_min = -1;
int real_max = 0;

/***********************************************************************
  Cache
 ***********************************************************************/
int cache_min = -1;
int cache_max = 0;

// Interface between disksim & fsim 

void reset_flash_stat()
{
  flash_read_num = 0;
  flash_write_num = 0;
  flash_gc_read_num = 0;
  flash_gc_write_num = 0; 
  flash_erase_num = 0;
  flash_oob_read_num = 0;
  flash_oob_write_num = 0; 
}

FILE *fp_flash_stat;
FILE *fp_gc;
FILE *fp_gc_timeseries;
double gc_di =0 ,gc_ti=0;


double calculate_delay_flash()
{
  double delay;
  double read_delay, write_delay;
  double erase_delay;
  double gc_read_delay, gc_write_delay;
  double oob_write_delay, oob_read_delay;

  oob_read_delay  = (double)OOB_READ_DELAY  * flash_oob_read_num;
  oob_write_delay = (double)OOB_WRITE_DELAY * flash_oob_write_num;

  read_delay     = (double)READ_DELAY  * flash_read_num; 
  write_delay    = (double)WRITE_DELAY * flash_write_num; 
  erase_delay    = (double)ERASE_DELAY * flash_erase_num; 

  gc_read_delay  = (double)GC_READ_DELAY  * flash_gc_read_num; 
  gc_write_delay = (double)GC_WRITE_DELAY * flash_gc_write_num; 


  delay = read_delay + write_delay + erase_delay + gc_read_delay + gc_write_delay + 
    oob_read_delay + oob_write_delay;

  if( flash_gc_read_num > 0 || flash_gc_write_num > 0 || flash_erase_num > 0 ) {
    gc_ti += delay;
  }
  else {
    gc_di += delay;
  }

  if(warm_done == 1){
    fprintf(fp_gc_timeseries, "%d\t%d\t%d\t%d\t%d\t%d\n", 
      req_count_num, merge_switch_num - old_merge_switch_num, 
      merge_partial_num - old_merge_partial_num, 
      merge_full_num - old_merge_full_num, 
      flash_gc_read_num,
      flash_erase_num);

    old_merge_switch_num = merge_switch_num;
    old_merge_partial_num = merge_partial_num;
    old_merge_full_num = merge_full_num;
    req_count_num++;
  }

  reset_flash_stat();

  return delay;
}

void UpdateCnt(int evict_mode)
{
 switch(evict_mode){
 	case 1:  index_wr_cnt++; break;
 	case 0:  strip_wr_cnt++; break;
 	default: break;}
}

/***********************************************************************
  Initialize Flash Drive 
  ***********************************************************************/
  
void flash_array_setup(char **argv)
{
 FILE *fp1 = fopen(argv[5], "r");
 char buffer[80];
 char para_type[20];
 long int para;
 int i;
 
 for(i=0;i<12;i++)
 {
 	if(fgets(buffer, sizeof(buffer), fp1))
    {sscanf(buffer, "%s = %d\n", para_type, &para);}

  switch(i)
  {
   case 0:
     {if(strcmp(para_type, "MaxBlkno") != 0)
     	{printf("MaxBlkno type is NOT correct!!\n"); exit(-1);}
      maxblk = para; break;
     }
   case 1:
     {if(strcmp(para_type, "FTL_BUF_NUM") != 0)
     	{printf("FTL_BUF_NUM type is NOT correct!!\n"); exit(-1);}
      FTL_BUF_NUM = para; break;
     }     
   case 2:
     {if(strcmp(para_type, "FTL_BUF_THRU") != 0)
     	{printf("FTL_BUF_THRU type is NOT correct!!\n"); exit(-1);}
      FTL_BUF_THRU = para; break;
     }     
   case 3:
     {if(strcmp(para_type, "FTL_CHNL_NUM") != 0)
     	{printf("FTL_CHNL_NUM type is NOT correct!!\n"); exit(-1);}
      FTL_CHNL_NUM = para; break;
     }
   case 4:
     {if(strcmp(para_type, "FTL_WAY_NUM") != 0)
     	{printf("FTL_WAY_NUM type is NOT correct!!\n"); exit(-1);}
      FTL_WAY_NUM = para; break;
     }     
   case 5:
     {if(strcmp(para_type, "FTL_CHNL_WIDTH") != 0)
     	{printf("FTL_CHNL_WIDTH type is NOT correct!!\n"); exit(-1);}
      FTL_CHNL_WIDTH = para; break;
     }  
   case 6:
     {if(strcmp(para_type, "FTL_WAY_WIDTH") != 0)
     	{printf("FTL_WAY_WIDTH type is NOT correct!!\n"); exit(-1);}
      FTL_WAY_WIDTH = para; break;
     }      
   case 7:
     {if(strcmp(para_type, "CACHE_HIT_ON") != 0)
     	{printf("CACHE_HIT_ON type is NOT correct!!\n"); exit(-1);}
      cache_policy = para; break;
     }  
   case 8:
     {if(strcmp(para_type, "RAID_PAGE_PER_STRIP") != 0)
     	{printf("RAID_PAGE_PER_STRIP type is NOT correct!!\n"); exit(-1);}
      page_per_strip = para; break;
     }       
   case 9:
     {if(strcmp(para_type, "VISIBLE_MAX_BLKNO") != 0)
     	{printf("VISIBLE_MAX_BLKNO type is NOT correct!!\n"); exit(-1);}
      visible_blkno = para; parity_blkno = visible_blkno + visible_blkno/1000; break;
     }       
   case 10:
     {if(strcmp(para_type, "DDA_REC_BUF_NUM") != 0)
     	{printf("DDA_REC_BUF_NUM type is NOT correct!!\n"); exit(-1);}
      rec_buf_num = para; break;
     }                          
   case 11:
     {if(strcmp(para_type, "EVICT_THRES") != 0)
     	{printf("EVICT_THRES type is NOT correct!!\n"); exit(-1);}
      EVICT_THRES = para; break;
     }        
   	    
   default: break;
  }
 }
 FTL_CE_NUM = FTL_CHNL_NUM * FTL_WAY_NUM;
 fclose(fp1);
}

static void allocftliob ()
{
   int i;
   fltiob *ftltemp = NULL;
   ftlsub *ftltemp1 = NULL;
   
   int struct_size;
   long int ftlbuf_size;
   struct_size = FTL_BUF_NUM * (sizeof(fltiob)); 
   ftlbuf_size = (sizeof(ftlqueue));
   
   //alloc for FltBuf
   FtlBuf  = (ftlqueue *)malloc(ftlbuf_size); 
   //alloc for stat
   FtlStat = (ftlstat_t *)malloc(sizeof(ftlstat_t));
   FtlStat->TotalLpNum = 0;
   FtlStat->TotalHitNum = 0;
   FtlStat->Total_NVstatic_energy    = 0;
   FtlStat->Total_NVaccess_energy    = 0;
   FtlStat->Total_Flashstatic_energy = 0;
   FtlStat->Total_Flashaccess_energy = 0;
   FtlStat->Total_Dramstatic_energy  = 0;
   FtlStat->Total_Dramaccess_energy  = 0; 
   FtlStat->Total_Dramrefresh_energy = 0;
   FtlStat->Total_Simtime            = 0; 
   FtlStat->Total_IoReq              = 0; 
      
   //alloc and initialize iobuf
   if ((ftltemp = (fltiob *)malloc(struct_size)) == NULL) {
      printf ("*** error: failed to allocate space for ftl iobuf\n");
      exit(1);
   }
   for (i=0; i<(FTL_BUF_NUM-1); i++) {
      ftltemp[i].next = &ftltemp[i+1];
      ftltemp[i].prev = NULL;   
      ftltemp[i].arrivetime = 0;
      ftltemp[i].starttime = 0; 
      ftltemp[i].availtime = 0;       
      ftltemp[i].bufcpltime = 0; 
      ftltemp[i].curr_flag = 0;
      ftltemp[i].state = FTL_BUF_FREE;      
   }
   ftltemp[(FTL_BUF_NUM-1)].next = NULL;
   FtlBuf->AvailIob_head = ftltemp;
   if(FTL_BUF_NUM == 1)
   {
   FtlBuf->AvailIob_tail = ftltemp; 
   }
   else
   {
   FtlBuf->AvailIob_tail = &ftltemp[(FTL_BUF_NUM-1)];
   }
   
   //alloc for subqueue
   struct_size = FTL_CHNL_NUM * (sizeof(ftlsub)); 
   if ((ftltemp1 = (ftlsub *)malloc(struct_size)) == NULL) {
      printf ("*** error: failed to allocate space for ftl SUBQUEUE\n");
      exit(1);
   } 
   for (i=0; i<FTL_CHNL_NUM; i++) 
   {
    FtlBuf->subqueue[i] = &ftltemp1[i];//need to change if chnl is greater than 1
   }    
}

void InitRaidIndex()
{
	int i,j;
	sie_t *temp;
	fltiob *temp2;
	/****************initialize index table
	****************/
  dda_strip_table = (sit_t *)malloc((sizeof(sit_t)));;
  dda_strip_table->TotalNum  = 0;
  dda_strip_table->UsedHead  = NULL;  
  dda_strip_table->UsedTail  = NULL;
  dda_strip_table->AvailHead  = NULL;  
  dda_strip_table->AvailTail  = NULL;

  temp = (sie_t *)malloc((rec_buf_num)*(sizeof(sie_t)));  
  for(i=0;i<(rec_buf_num);i++){
    temp[i].index_no = i;
    temp[i].wr_strip_num = 0;
    for(j=0;j<page_per_strip;j++){temp[i].vld[j] = 0;}
    if(i==0)temp[i].prev = NULL;
    else temp[i].prev = &temp[i-1];    
    if(i == (rec_buf_num-1))
      temp[i].next = NULL;
    else temp[i].next = &temp[i+1];}
    dda_strip_table->AvailHead = temp;
   if(rec_buf_num == 1)
    {dda_strip_table->AvailTail = temp;}
   else
    {dda_strip_table->AvailTail = &temp[rec_buf_num-1];}    
}	

void InitPageTableBuf()
{
	int i;
	entry_t *temp;
  ppc_table  = (pct_t *)malloc((sizeof(pct_t)));
  ppc_table->TotalNum  = 0;
  ppc_table->UsedHead  = NULL;  
  ppc_table->UsedTail  = NULL;
  ppc_table->AvailHead  = NULL;  
  ppc_table->AvailTail  = NULL;
  
  for(i=0;i<FTL_BUF_NUM/SECT_NUM_PER_PAGE;i++){
    temp = (entry_t *)malloc((sizeof(entry_t)));
    temp->TotalOldPBN = 0;
    temp->ParityLPN = i;    
    temp->AccCnt = 0;    
    temp->prev = NULL;    
    temp->next = NULL;
    if(ppc_table->AvailHead == NULL){
      ppc_table->AvailHead = temp; ppc_table->AvailTail = temp;	
    }
    else{
      ppc_table->AvailTail->next = temp;
      temp->prev = ppc_table->AvailTail;
      ppc_table->AvailTail = temp; 
    }
   }

  gc_page_table  = (pct_t *)malloc((sizeof(pct_t)));
  gc_page_table->TotalNum  = 0;  
  gc_page_table->UsedHead  = NULL;  
  gc_page_table->UsedTail  = NULL;

}

void InitIobState ()
{
   int i;
   int j;
   ftlway  *waytemp;
   allocftliob ();//get subqueue
   
   FtlBuf->TotalBufSize = FTL_BUF_NUM;
   FtlBuf->AvailBufSize = FTL_BUF_NUM;
   FtlBuf->MaxReqBufSize= FTL_BUF_NUM;
   FtlBuf->UsedBufSize = 0;
   FtlBuf->UsedIob_head = NULL;
   FtlBuf->UsedIob_tail = NULL; 
   FtlBuf->OutstandingIobNum = 0;
   FtlBuf->MaxChnlNum = FTL_CHNL_NUM;
   FtlBuf->CwqPointer = 0;
             
   for(i=0; i<FTL_CHNL_NUM;i++)//initialize ways
   {  	
    FtlBuf->subqueue[i]->MaxWayNum = FTL_WAY_NUM;
    FtlBuf->subqueue[i]->WaysTransCplTime = 0;
    waytemp = (ftlway *)malloc(sizeof(ftlway)*FTL_WAY_NUM);     
    for(j=0;j<FTL_WAY_NUM;j++)
    {
     FtlBuf->subqueue[i]->wayinfo[j] = &waytemp[j];    	
     FtlBuf->subqueue[i]->wayinfo[j]->CplTime = 0;
     FtlBuf->subqueue[i]->wayinfo[j]->CurrArriveTime = 0;     
     FtlBuf->subqueue[i]->wayinfo[j]->CurrCpltransTime = 0; 
     FtlBuf->subqueue[i]->wayinfo[j]->state = FTL_WAY_FREE;         
    }
   }
}


void initFlash(char **argv)
{
  blk_t total_blk_num_per_chip;
  blk_t total_util_blk_num_per_chip;
  blk_t total_extra_blk_num_per_chip;  
  blk_t glabal_total_extr_blk_num;
  int i;

  flash_array_setup(argv);
 
  // total number of sectors    
  total_util_sect_num  = flash_numblocks;
  total_extra_sect_num = flash_extrblocks;
  total_sect_num = total_util_sect_num + total_extra_sect_num; 

  // total number of blocks 
  total_blk_num_per_chip      = total_sect_num/(SECT_NUM_PER_BLK*FTL_CE_NUM);            // total block number per num
  total_util_blk_num_per_chip = total_util_sect_num/(SECT_NUM_PER_BLK*FTL_CE_NUM);       // total unique block number per num
  total_extra_blk_num_per_chip  = total_extra_sect_num/(SECT_NUM_PER_BLK*FTL_CE_NUM);
  
  global_total_blk_num = total_util_sect_num/SECT_NUM_PER_BLK;                           //total block number

  glabal_total_extr_blk_num = total_extra_sect_num/SECT_NUM_PER_BLK;                     // total extra block number

  ASSERT(flash_extrblocks != 0);

  if (nand_init(total_blk_num_per_chip, FREE_BLK_LOWLEVEL, total_extra_blk_num_per_chip) < 0) {
    EXIT(-4); 
  }
  
  switch(ftl_type){

    // pagemap
    case 1: ftl_op = pm_setup(); break;
    //temporarily remove the remaining ftl type
    default: break;
  }
   
  ftl_op->init(global_total_blk_num, glabal_total_extr_blk_num);
  nand_stat_reset(); 
  InitIobState();
  switch(cache_policy){
  	case NO_CACHE://NO CACHE
  	case RAID5:
  	case LRU:break;
  	case DDA:
      InitRaidIndex();//initialize index table, entry table and rec_buf
      break; 
  	case HDA:
      InitRaidIndex();
      InitPageTableBuf();      
      break;            
    case PPC: 
    case CHUNKPPC:
  	case PBARAID:    	
      InitPageTableBuf();
      break;     
    default: break;
  }
}

void printWearout()
{
  int i;
  int j;
  ftlnandflash * currfmem;
  FILE *fp = fopen("wearout", "w");
  
  for(i = 0; i<FTL_CE_NUM; i++)
  { 
    currfmem = FtlGetFlashmem(i);
    
    for(j = 0; j<currfmem->nand_blk_num; j++)
    {
    fprintf(fp, "%d %d %d\n", i, j, currfmem->nand_blk[i].state.ec); 
    }
  }  
  fclose(fp);
 }

void endFlash()
{
  nand_stat_print(outputfile);
  FtlPrintStat(outputfile);
  ftl_op->end;
  nand_end();
}  

/***********************************************************************
  Send request (lsn, sector_cnt, operation flag)
  ***********************************************************************/

void send_flash_request(int start_blk_no, int block_cnt, int operation, int mapdir_flag, unsigned int ftlcenum)
{
	int size;
	int currcenum;
	//size_t (*op_func)(sect_t lsn, size_t size);
	size_t (*op_func)(sect_t lsn, size_t size, int mapdir_flag, unsigned int ftlcenum);

        if((start_blk_no + block_cnt) >= total_util_sect_num){
          printf("start_blk_no: %d, block_cnt: %d, total_util_sect_num: %d\n", 
              start_blk_no, block_cnt, total_util_sect_num);
          exit(0);
        }
        
	switch(operation){

	//write
	case 0:

		op_func = ftl_op->write;
		while (block_cnt> 0) {
			currcenum = FtlGetCenum(start_blk_no);
			size = op_func(start_blk_no, block_cnt, mapdir_flag, currcenum);
			start_blk_no += size;
			block_cnt-=size;
		}
		break;
	//read
	case 1:


		op_func = ftl_op->read;
		while (block_cnt> 0) {
			currcenum = FtlGetCenum(start_blk_no);			
			size = op_func(start_blk_no, block_cnt, mapdir_flag, currcenum);
			start_blk_no += size;
			block_cnt-=size;
		}
		break;

	default: 
		break;
	}
}

//void find_real_max()
//{
//  int i; 
//
//  for(i=0;i < MAP_REAL_MAX_ENTRIES; i++) {
//      if(opagemap[real_arr[i]].map_age > opagemap[real_max].map_age) {
//          real_max = real_arr[i];
//      }
//  }
//}

//void find_real_min()
//{
//  
//  int i,index; 
//  int temp = 99999999;
//
//  for(i=0; i < MAP_REAL_MAX_ENTRIES; i++) {
//        if(opagemap[real_arr[i]].map_age <= temp) {
//            real_min = real_arr[i];
//            temp = opagemap[real_arr[i]].map_age;
//            index = i;
//        }
//  }    
//}

//int find_min_ghost_entry()
//{
//  int i; 
//
//  int ghost_min = 0;
//  int temp = 99999999; 
//
//  for(i=0; i < MAP_GHOST_MAX_ENTRIES; i++) {
//    if( opagemap[ghost_arr[i]].map_age <= temp) {
//      ghost_min = ghost_arr[i];
//      temp = opagemap[ghost_arr[i]].map_age;
//    }
//  }
//  return ghost_min;
//}

//void init_arr()
//{
//
//  int i;
//  for( i = 0; i < MAP_REAL_MAX_ENTRIES; i++) {
//      real_arr[i] = -1;
//  }
//  for( i = 0; i < MAP_GHOST_MAX_ENTRIES; i++) {
//      ghost_arr[i] = -1;
//  }
//  for( i = 0; i < CACHE_MAX_ENTRIES; i++) {
//      cache_arr[i] = -1;
//  }
//
//}
//
//int search_table(int *arr, int size, int val) 
//{
//    int i;
//    for(i =0 ; i < size; i++) {
//        if(arr[i] == val) {
//            return i;
//        }
//    }
//
//    printf("shouldnt come here for search_table()=%d,%d",val,size);
//    for( i = 0; i < size; i++) {
//      if(arr[i] != -1) {
//        printf("arr[%d]=%d ",i,arr[i]);
//      }
//    }
//    exit(1);
//    return -1;
//}
//
//int find_free_pos( int *arr, int size)
//{
//    int i;
//    for(i = 0 ; i < size; i++) {
//        if(arr[i] == -1) {
//            return i;
//        }
//    } 
//    printf("shouldnt come here for find_free_pos()");
//    exit(1);
//    return -1;
//}

//void find_min_cache()
//{
//  int i; 
//  int temp = 999999;
//
//  for(i=0; i < CACHE_MAX_ENTRIES ;i++) {
//      if(opagemap[cache_arr[i]].cache_age <= temp ) {
//          cache_min = cache_arr[i];
//          temp = opagemap[cache_arr[i]].cache_age;
//      }
//  }
//}

int youkim_flag1=0;

double callFsim(unsigned int secno, int scount, int operation, unsigned int ftlcenum)
{
  double delay; 
  int bcount;
  unsigned int blkno; // pageno for page based FTL
  int cnt,z; int min_ghost;

  int pos=-1,pos_real=-1,pos_ghost=-1;

  if(ftl_type == 1){ }
      
  // page based FTL 
  if(ftl_type == 1 ) { 
    blkno = secno / SECT_NUM_PER_PAGE;
    bcount = (secno + scount -1)/SECT_NUM_PER_PAGE - (secno)/SECT_NUM_PER_PAGE + 1;
  }  

  cnt = bcount;

  switch(operation)
  {
    //write/read
    case 0:
    case 1:

    while(cnt > 0)
    {
          cnt--;

        // page based FTL
        if(ftl_type == 1){
          send_flash_request(blkno*SECT_NUM_PER_PAGE, SECT_NUM_PER_PAGE, operation, 1, ftlcenum); 
          blkno++;
        }
    }
    break;
  }

  delay = calculate_delay_flash();

  return delay;
}

/***********************************************************************
  for io buffer part    
 ***********************************************************************/
 ftlsub * FtlGetChnlInfo( unsigned int ftlcenum)
 {
  //check whether the io length exceed the allowed length: for debug and further usage
  ftlsub *currchnl;
  int chnlno;
    
  chnlno = ftlcenum%FTL_CHNL_NUM;//way-->chnl
  if(chnlno > FtlBuf->MaxChnlNum)
    {
     printf("channel number exceeds for FtlGetWayInfo\n");
     exit(1);
    }   
  currchnl = FtlBuf->subqueue[chnlno];
  return(currchnl);
 } 
 
 ftlway * FtlGetWayInfo( unsigned int ftlcenum, int bcount)
 {
  //check whether the io length exceed the allowed length: for debug and further usage
  ftlsub *currchnl;
  int waynno;
      
  currchnl = FtlGetChnlInfo(ftlcenum);
  waynno = ftlcenum>>FTL_CHNL_WIDTH;
  if(waynno > currchnl->MaxWayNum)
    {
     printf("way number exceeds for FtlGetWayInfo\n");
     exit(1);
    }  
  
  return(currchnl->wayinfo[waynno]);
 }
 
 int FtlChkIobfnum( int bcount)
 {
    
  if(bcount > FtlBuf->MaxReqBufSize)
    {
     printf("IO length of %d exceeds the allowed maximum length of %d\n", bcount,FtlBuf->MaxReqBufSize);
     exit(1);
    }
     	
  if(FtlBuf->AvailBufSize >= (unsigned int)bcount)
    {
     return(1);
    }
  else
    {
    return(0);
    }
 }

fltiob * FtlGetNewIob(int bcount)
{
 fltiob * temp = NULL;
 	
 if(FtlBuf->AvailBufSize == 0 || (unsigned int)FtlBuf->AvailBufSize < (unsigned int)bcount)
   {
    printf("Error: Ftl simulator doesn't have enough buffer/n");
    exit(1);
   }
 else
   {
    FtlBuf->AvailBufSize -= bcount;

    FtlBuf->UsedBufSize += bcount;
    
    if(FtlBuf->AvailBufSize + FtlBuf->UsedBufSize != FtlBuf->TotalBufSize)
      {
       printf("Error: Ftl simulator AvailBufSize and UsedBufSize number is inconsistent with TotalBufSize for FtlGetNewIob\n");
       exit(1);      	
      }
    if((FtlBuf->UsedBufSize > FtlBuf->TotalBufSize) || (FtlBuf->AvailBufSize > FtlBuf->TotalBufSize))
      {
       printf("Error: Ftl simulator AvailBufSize and UsedBufSize exceed for FtlGetNewIob\n");
       exit(1);      	
      }    
    if(FtlBuf->AvailIob_head == NULL)
      {
       printf("Error: Ftl simulator has enough buffer but iobuf pool has no buf loc\n");
       exit(1);      	
      }
    else
      {
       temp = FtlBuf->AvailIob_head;
       if((FtlBuf->AvailIob_tail == FtlBuf->AvailIob_head) && (temp->next != NULL))//the last iobuf
       {
       printf("Error: Ftl simulator AvailIobuf_tail->next pointer is not correct! for FtlGetNewIob\n");
       exit(1);  
       }       
       FtlBuf->AvailIob_head = temp->next;
       if(FtlBuf->AvailIob_tail == temp)//the last iobuf
       {
       	FtlBuf->AvailIob_tail = NULL;
       }
       temp->next = NULL;
       if(temp->state != FTL_BUF_FREE){
         printf("current iobuf state is not FTL_BUF_FREE before it is released for FtlGetNewIob\n");
         exit(-1); }       
       return(temp);   	
      }    
   }
}


//fltiob * FtlGetPendingIob()
//{
// fltiob * temp = NULL;
// 	
// if(FtlBuf->OutstandingIobNum == 0)
//   {
//    printf("Error: Ftl subqueue has no outstanding io for FtlGetPendingIob\n");
//    exit(1);
//   }
// else if(FtlBuf->UsedIob_head == NULL || FtlBuf->UsedIob_tail == NULL)
//   {
//    printf("Error: Ftl subqueue head pointer is empty for FtlGetPendingIob\n");
//    exit(1);      	
//   }
// else
//   {
//    temp = FtlBuf->UsedIob_head;
//    if(temp->state != FTL_BUF_DATA_ARRIVE && (temp->flags&READ) == 0)//write
//    {
//     printf("temp->state is not FTL_BUF_DATA_ARRIVE for for FtlGetPendingIob\n");
//     exit(1);      	
//    } 
//    if(temp->state != FTL_BUF_WAIT_DATA && (temp->flags&READ) == 1)//read
//    {
//     printf("temp->state is not FTL_BUF_WAIT_DATA for for FtlGetPendingIob\n");
//     exit(1);      	
//    }      
//    if((temp->arrivetime >= temp->starttime && (temp->flags&READ) == 0)|| //write
//       (temp->arrivetime != temp->starttime && (temp->flags&READ) == 1))  //read
//    {
//     printf("arrivetime and starttime are disordered for FtlGetPendingIob\n");
//     exit(1);      	
//    }             
//     return(temp);        	   
//   }
//}

int ChkStripNum(unsigned int lpn)
{
 entry_t * temp;
 entry_t * temp1 = NULL;

 unsigned int parity_lpn;
 parity_lpn = lpn/page_per_strip + parity_blkno/SECT_NUM_PER_PAGE;
 
 if(ppc_table->UsedHead == NULL)
   {printf("remove parity from the empty in ChkStripNum!!\n"); exit(-1);}
 if(ppc_table->UsedHead == NULL && ppc_table->UsedTail != NULL)
   {printf("ppc_table->UsedTail and ppc_table->UsedHead are not consistent in ChkStripNum!!\n"); exit(-1);} 	        
 if(ppc_table->TotalNum == 0)
   {printf("TotalNum is NOT correct in ChkStripNum!!\n"); exit(-1);} 
   	        
 temp = ppc_table->UsedHead;
 while(temp){
   if(temp->ParityLPN == parity_lpn && temp1 == NULL) temp1 = temp;
   else if(temp->ParityLPN == parity_lpn && temp1 != NULL){printf("Multiple cache entries record the same parity lpn in ChkStripNum!!\n"); exit(-1);}
   temp = temp->next;   	
   }
 if(temp1 == NULL)  
 	{printf("Cannot find matched strip in ChkStripNum!!\n"); exit(-1);}
 else return(temp1->TotalOldPBN);
}   

fltiob * FtlGetLRUPendingIob()
{
 fltiob * temp = NULL;
 fltiob * temp1 = NULL;
 double   temptime;
 int i;
 	
 if(FtlBuf->OutstandingIobNum == 0)
   {
    printf("Error: Ftl subqueue has no outstanding io for FtlGetLRUPendingIob\n");
    exit(1);
   }
 else if(FtlBuf->UsedIob_head == NULL || FtlBuf->UsedIob_tail == NULL)
   {
    printf("Error: Ftl subqueue head pointer is empty for FtlGetLRUPendingIob\n");
    exit(1);      	
   }
 else
   {
   	temp = FtlBuf->UsedIob_head;
   	temp1 = FtlBuf->UsedIob_head; 
   	temptime = temp->arrivetime;
   	for(i=0; i<(FtlBuf->OutstandingIobNum);i++)
   	{
     
     if(temp->state != FTL_BUF_DATA_ARRIVE && ((temp->flags & READ) == 0) &&
     	  temp->curr_flag == 0)//write
     {
      printf("temp->state is not FTL_BUF_DATA_ARRIVE for FtlGetLRUPendingIob\n");
      exit(1);      	
     } 
     if(temp->state != FTL_BUF_TX_CPL && ((temp->flags & READ) == 1) && 
     	  temp->curr_flag == 0)//read
     {
      printf("temp->state is not FTL_BUF_CPL for FtlGetLRUPendingIob\n");
      exit(1);      	
     }           
     if(temp->arrivetime < temptime && temp->curr_flag == 0)//not current io req 
     {
         temp1 = temp; 
         temptime = temp->arrivetime; 	
     }  
     if(i == (FtlBuf->OutstandingIobNum-1) && temp != FtlBuf->UsedIob_tail)
     {
      printf("Error: OutstandingIobNum is not correct for FtlGetLRUPendingIob\n");
      exit(1);     	
     }
     temp = temp->next;            
    } 
    return(temp1);      	   
   }
}

fltiob * FtlGetWaitingIob()
{
 fltiob * temp = NULL;
 	
 if(FtlBuf->OutstandingIobNum == 0)
   {
    printf("Error: Ftl Usedqueue has no outstanding io for FtlGetWaitingIob\n");
    exit(1);
   }
 else if(FtlBuf->UsedIob_head == NULL || FtlBuf->UsedIob_head == NULL)
   {
    printf("Error: Ftl subqueue head pointer is empty for FtlGetWaitingIob\n");
    exit(1);      	
   }
 else
   {
    temp = FtlBuf->UsedIob_tail;
    if(temp->state != FTL_BUF_WAIT_DATA)
    {
     printf("temp->state is not FTL_BUF_WAIT_DATA for FtlGetWaitingIob\n");
     exit(1);      	
    }         
     return(temp);        	   
   }
}

void FtlInsertUsedIob(unsigned int block, int flags, fltiob * curriob)
{
   fltiob * temp;
   if(curriob->availtime < simtime)
     {
      curriob->arrivetime = simtime;
      curriob->starttime  = simtime;
      curriob->bufcpltime = simtime;
     }
   else
   	 {
      curriob->arrivetime = curriob->availtime;
      curriob->starttime  = curriob->availtime; 
      curriob->bufcpltime = curriob->availtime;  	 	
   	 }
   if(curriob->state != FTL_BUF_FREE)
    {
     printf("current iobuf hasn't been released before it is used agian for InsertIobIntoUsedqueue\n");
     exit(1);
    }  
   else
    {
    curriob->state = FTL_BUF_WAIT_DATA;
    }
    
   if(curriob->curr_flag != 0)
    {
     printf("curr_flag is not correct for FtlInsertUsedIob\n");
     exit(1);
    }     
   
   curriob->next = NULL;
   curriob->prev = NULL;
   curriob->bcount = SECT_NUM_PER_PAGE;
   curriob->devno = 0;
   curriob->blkno = block;
   curriob->flags = flags & READ;
   
   switch(cache_policy){
   	 case LRU: 
   	 case DDA:
   	 case HDA:{
       curriob->curr_flag = 1;break;}
     case PPC: 
     case CHUNKPPC:
     	 curriob->state = FTL_BUF_DATA_ARRIVE;break;
     default:break;
   }
   if((FtlBuf->UsedIob_head == NULL && FtlBuf->UsedIob_tail != NULL)||//OutstandingNum is inconsistent with head pointer 
      (FtlBuf->UsedIob_head == NULL && FtlBuf->OutstandingIobNum != 0) )
   {
     printf("Error:req outstanding number is inconsitent with head pointer(NULL), OutstandingIobNum is %d for InsertIobIntoUsedqueue\n",FtlBuf->OutstandingIobNum);
     exit(1);    
   }
   else if(FtlBuf->UsedIob_head == NULL)//the first one
    {
     FtlBuf->UsedIob_head = curriob;
     FtlBuf->UsedIob_tail = curriob;
    }
   else//not the first io
    {
     temp = FtlBuf->UsedIob_tail;
     FtlBuf->UsedIob_tail = curriob;
     temp->next = curriob;
     curriob->prev = temp; 	
    }
   FtlBuf->OutstandingIobNum++;
   
   switch(cache_policy){
   	case LRU:
   	case DDA:
   	case HDA:{
      FtlBuf->CurrWaitQueue[FtlBuf->CwqPointer] = curriob;
      FtlBuf->CwqPointer++; break;}
    default: break;
   }
} 

unsigned int FtlGetCenum(unsigned int blkno)
{
 unsigned int cenum;

 cenum = blkno/SECT_NUM_PER_PAGE; 
 cenum = cenum%FTL_CE_NUM;
 return(cenum);
}

void AddToIobqueue(fltiob *curriob)
{
 fltiob *temp;
 //first check info
 if(FtlBuf->TotalBufSize == FtlBuf->AvailBufSize)
 {
  printf("Error:exceed buf is to be added in iobuf for AddToIobqueue\n");
  exit(1); 	
 } 
 //add to iob queue
 
 if(FtlBuf->AvailIob_head == NULL)//iob queue is empty
 {
  curriob->next = NULL;
  FtlBuf->AvailIob_head = curriob; 
  FtlBuf->AvailIob_tail = curriob;  
 } 
 else
 {
  temp = FtlBuf->AvailIob_tail;
  temp->next = curriob;
  FtlBuf->AvailIob_tail = curriob;	
 }
 
 FtlBuf->AvailBufSize += curriob->bcount;
 if(FtlBuf->AvailBufSize > FtlBuf->TotalBufSize)
 {
  printf("Error:exceed buf has been added in iobuf for AddToIobqueue\n");
  exit(1); 	
 }  

 if(FtlBuf->UsedBufSize < curriob->bcount)
 {
  printf("Error:used buffer size is less than curriob size for AddToIobqueue\n");
  exit(1); 	
 } 
 FtlBuf->UsedBufSize -= curriob->bcount;
 //printf("Return %d blk and AvailBufSize is %d\n",curriob->bcount,FtlBuf->AvailBufSize);      
}
//void FtlRemovedCplIob(fltiob *curriob)
//{
// //check state
// if(curriob->state != FTL_BUF_CPL)
// {
//  printf("current iobuf state is not FTL_BUF_CPL before it is released for FtlRemovedCplIob\n");
//  exit(1); 	
// }
//
//  if(FtlBuf->UsedIob_head != curriob)
//  {
//  printf("Error:subqueue head pointer is inconsistent with curriob for FtlRemovedCplIob\n");
//  exit(1);    
//  } 
//  curriob->state = FTL_BUF_FREE;
//  FtlBuf->UsedIob_head = curriob->next;
//  curriob->next = NULL;  
//  if(FtlBuf->UsedIob_tail == curriob)//last io in the subqueue 
//   { 
//    FtlBuf->UsedIob_tail = NULL;
//    if(FtlBuf->OutstandingIobNum != 1)
//    {
//     printf("Error: Outstanding io number is inconsistent with tail pointer for FtlRemovedCplIob\n");
//     exit(1);      	
//    }     
//   }
//   
//  FtlBuf->OutstandingIobNum--; 
//  //add current iob into iob queue
//  AddToIobqueue(curriob);
//}
 
//for cache hit
void FtlRemovedLRUCplIob(fltiob *curriob)
{
 //check state
 if(curriob->state != FTL_BUF_CPL)
 {
  printf("current iobuf state is not FTL_BUF_CPL before it is released for FtlRemovedLRUCplIob\n");
  exit(1); 	
 }

//  if(FtlBuf->UsedIob_head != curriob)
//  {
//  printf("Error:subqueue head pointer is inconsistent with curriob for FtlRemovedLRUCplIob\n");
//  exit(1);    
//  } 
  curriob->state = FTL_BUF_FREE;

  if(FtlBuf->UsedIob_head == curriob)
   {
    FtlBuf->UsedIob_head = curriob->next;    
   }
  else
   {
   	curriob->prev->next = curriob->next;
   } 
    
  if(FtlBuf->UsedIob_tail == curriob)
   {
    FtlBuf->UsedIob_tail = curriob->prev;    
   }
  else
   {
   	curriob->next->prev = curriob->prev;
   }
   
  curriob->next = NULL;  
  curriob->prev = NULL; 
   
  FtlBuf->OutstandingIobNum--; 
  if(FtlBuf->OutstandingIobNum == 0 && 
  	(FtlBuf->UsedIob_tail != NULL || FtlBuf->UsedIob_head != NULL))//last io in ioqueue
   { 
    printf("Error: Outstanding io number is inconsistent with tail pointer for FtlRemovedCplIob\n");
    exit(1);      	
   }     
   
  AddToIobqueue(curriob);
} 
 
//void FtlAddIntoSubqueue(ioreq_event *curr)
//{
// fltiob * curriob;	
// curriob = FtlGetNewIob(curr->bcount);
// InsertIobIntoUsedqueue(curr,curriob);
//}

fltiob * FtlAddNewUsedIob(unsigned int block, int flags)
{
 fltiob * curriob;	
 curriob = FtlGetNewIob(SECT_NUM_PER_PAGE);
 FtlInsertUsedIob(block,flags,curriob);
 return(curriob);
}

void AddWayInfo(fltiob *curriob, ftlway *currway, unsigned int bcount, unsigned int ftlcenum)
{ 
   ftlsub *currchnl;
   double temp;
   
   currchnl = FtlGetChnlInfo(ftlcenum);
   
 switch(cache_policy)
  {
   case NO_CACHE:
   case RAID5:
   case PBARAID:
   temp = max(simtime,currway->CplTime); break;
   case LRU:
   temp = max(curriob->starttime,currway->CplTime); break;
   case PPC: 
   case CHUNKPPC:
   case DDA:
   case HDA:
   	if(curriob != NULL) temp = max(curriob->starttime,currway->CplTime);
    else temp = max(simtime,currway->CplTime); break; 
   default:break;
  }
   temp = max(temp,currchnl->WaysTransCplTime);
   currchnl->WaysTransCplTime = FLASH_TRANSTIME + temp;
   currway->CurrArriveTime = temp;	
   
   if(currway->state!= FTL_WAY_FREE)
   {
    printf("Error: currway state is not FTL_WAY_FREE for AddWayInfo\n");
    exit(1);   	   	
   }
   currway->state = FTL_WAIT_DATA;
   currway->CurrCpltransTime = currway->CurrArriveTime; 
}

 double UpdateWayInfo(double overhead, ftlway *currway)
{
   double tdiff = 0;
   currway->CplTime = currway->CurrCpltransTime + overhead;
   if(currway->state!= FTL_WAIT_DATA)
   {
    printf("Error: currway state is not FTL_WAIT_DATA for UpdateWayInfo\n");
    exit(1);   	   	
   }
   currway->state = FTL_WAY_FREE;
   if(currway->CplTime > simtime)
   {
    tdiff = currway->CplTime - simtime;
   }
   return(tdiff); 
}

double FtlCplReq(unsigned int blkno,int flags,fltiob *curriob)
{
 double flashacctime = 0;
 double ftlchnlacctime = 0;
 ftlway * currway; 
 unsigned int ftlcenum;
 
 ftlcenum = FtlGetCenum(blkno);
 if(ftlcenum > FTL_CE_NUM)
 {
  printf("Error: ftl ce number exceeds for FtlCplReq\n");
  exit(1);   	
 } 
  
 currway = FtlGetWayInfo(ftlcenum,SECT_NUM_PER_PAGE);
 AddWayInfo(curriob, currway,SECT_NUM_PER_PAGE,ftlcenum);
 flashacctime = callFsim(blkno, SECT_NUM_PER_PAGE,(flags & READ),ftlcenum);
 ftlchnlacctime = UpdateWayInfo(flashacctime, currway);
 
 switch(cache_policy){
    case NO_CACHE: case RAID5:
    case PBARAID: 
    	break;
    case PPC:
    case CHUNKPPC: 
    	if(curriob != NULL){
       curriob->bufcpltime = curriob->starttime + flashacctime;//for write: bufcpltime are used to modify available time
       curriob->starttime = curriob->bufcpltime;
      } 
    	break;
    case LRU: case DDA:
    	{
    	 if(curriob != NULL){
         if((curriob->flags & READ) == 1){//read
          if(curriob->state != FTL_BUF_WAIT_DATA){
          printf("Error: curriob->state is not correct for FtlCplReq\n");
          exit(1);}
          curriob->state = FTL_BUF_DATA_ARRIVE;}
         curriob->bufcpltime = curriob->starttime + flashacctime;//for write: bufcpltime are used to modify available time
         curriob->starttime = curriob->bufcpltime;} 
       break;
      } 
    case HDA:
    	{
    	 if(curriob != NULL){
         if(flags == 1){//read
          if(curriob->state != FTL_BUF_WAIT_DATA){
          printf("Error: curriob->state is not correct for FtlCplReq\n");
          exit(1);}
          curriob->state = FTL_BUF_DATA_ARRIVE;}
         curriob->bufcpltime = curriob->starttime + flashacctime;//for write: bufcpltime are used to modify available time
         curriob->starttime = curriob->bufcpltime;} 
       break;
      }       
    default:break;
  }
 return(ftlchnlacctime); 	
}

int ChkModPageNum(unsigned int lpn)
{
	int page_num = -1;
	int i;
	entry_t *temp;
	temp = ppc_table->UsedHead;
	for(i=0;i<ppc_table->TotalNum;i++)
	 {
	 	if(temp == NULL){printf("TotalNum is not consistent with usedlink!!\n"); exit(-1);}
	 	if(temp->ParityLPN == lpn) page_num = temp->TotalOldPBN; 
	 	temp = temp->next;}
	if(page_num == -1){printf("cannot find matched lpn in ChkModPageNum!!\n"); exit(-1);}
	return(page_num);
}

unsigned int ChkPPCOldPPN(unsigned int lpn)
{
	unsigned int ppn = 0;
	int i;
	int j;
	entry_t *temp;
	entry_t *temp1;	
	unsigned int parity_lpn;
	
	parity_lpn = lpn/page_per_strip + parity_blkno/SECT_NUM_PER_PAGE;
	temp = ppc_table->UsedHead;
	for(i=0;i<ppc_table->TotalNum;i++)
	 {
	 	if(temp == NULL){printf("TotalNum is not consistent with usedlink!!\n"); exit(-1);}
    if(temp->TotalOldPBN == 0){printf("TotalOldPBN is NOT correct in ChkPPCOldPPN!!\n"); exit(-1);}  	
	 	if(temp->ParityLPN == parity_lpn){ 
	 	    for(j=0;j<temp->TotalOldPBN;j++){ 
	 	    	if(temp->ModifiedLPN[j] == lpn){
             if(ppn!=0){printf("Incorrect repeated hit in ChkPPCOldPPN!!\n"); exit(-1);}  		 	    		 
	 	    		 ppn = ((temp->OldPBN[j])<<1)|0x1;}
	 	    }}
	  temp = temp->next;
	 }
	if(temp!= NULL){printf("Link is inconsistent with tail pointer in ChkPPCOldPPN!!\n"); exit(-1);}
		 
	return(ppn);
}

double GetRaidReadOverhead(unsigned int lpn,int valid_flag)//for lsn
{
	double read_overhead = 0;
	double temp_overhead = 0;
	int modified_page_number = 0;
	unsigned int curr_read_lpn;
	unsigned int curr_read_ppn;
  unsigned int return_val;
  int cnt = 0; 
	int i;

	if(lpn >= parity_blkno/SECT_NUM_PER_PAGE)//parity	
	  curr_read_lpn = lpn;
	else
		curr_read_lpn = parity_blkno/SECT_NUM_PER_PAGE + lpn/page_per_strip;
		
	modified_page_number = ChkModPageNum(curr_read_lpn);
  curr_read_lpn = (curr_read_lpn - parity_blkno/SECT_NUM_PER_PAGE)*page_per_strip;
		
     for(i=0;i<page_per_strip;i++){
  	   return_val = ChkPPCOldPPN(curr_read_lpn);//return_val = curr_read_ppn << 1 | hit flag
  	   curr_read_ppn = return_val>>1; return_val = return_val & 0x1; 
  	   if(return_val == 1)cnt++;
	     if(modified_page_number >= page_per_strip/2){//read unmodified data   	   
  	      if(return_val == 0 && 
  	      	((curr_read_lpn == lpn && valid_flag != PPC_GC_VLD) || curr_read_lpn != lpn)){//valid data page & n > p/2  
  	      		temp_overhead = FtlCplReq(curr_read_lpn*SECT_NUM_PER_PAGE,READ,0);  
	            read_overhead = max(read_overhead,temp_overhead);}} 
	     else {
  	      if(return_val == 1 &&
  	      	((curr_read_lpn == lpn && valid_flag != PPC_GC_VLD) || curr_read_lpn != lpn)){//question: should read the orginal ppn. However, since the prev & curr ppn in the same chip, overhead is the same   
	           temp_overhead = FtlCplReq(curr_read_lpn*SECT_NUM_PER_PAGE,READ,0);
	           read_overhead = max(read_overhead,temp_overhead);}} 
	     curr_read_lpn = curr_read_lpn + 1;
	   }	
	 curr_read_lpn = (curr_read_lpn-1)/page_per_strip + parity_blkno/SECT_NUM_PER_PAGE; 
	 
	if(modified_page_number < page_per_strip/2 &&
		((curr_read_lpn == lpn && valid_flag != PPC_GC_VLD) || curr_read_lpn != lpn))//valid parity page & n <p/2
		{read_overhead = max(read_overhead,FtlCplReq(curr_read_lpn*SECT_NUM_PER_PAGE,READ,0));}
  if(cnt != modified_page_number){printf("modified_page_number is NOT correct in GetRaidReadOverhead!!\n"); exit(-1);}
	return(read_overhead);
}

double FtlGetIobOverhead(int valid_flag,unsigned int com_lpn)//release buffer
{
 fltiob * curriob;
 double ftlaccoverhead = 0;
 double raid_read_overhead = 0;
 unsigned int iob_blkno;
 unsigned int temp_lpn; 
 int iob_bcount;     

	if(com_lpn >= parity_blkno/SECT_NUM_PER_PAGE)//parity	
	  temp_lpn = com_lpn;
	else
		temp_lpn = parity_blkno/SECT_NUM_PER_PAGE + com_lpn/page_per_strip;

 //1: get iorequest
 switch(valid_flag){
  case NORMAL_LRU: curriob = FtlGetLRUPendingIob(); 
	                 if(curriob == NULL){printf("curriob is NOT correct in FtlGetIobOverhead!!\n"); exit(-1);} 
	                 break;
 	case PPC_GC_VLD: 
 	case PPC_GC_INVLD: curriob = FtlChkBufHit(com_lpn*SECT_NUM_PER_PAGE); break;
  default:break;}
 
 if(curriob != NULL)
 	{ 
    if(curriob->curr_flag == 1)
    {
     printf("Error: curr_flag is not correct for FtlCplReq\n");
     exit(1); 	
    }
    
    //2: get blk info
    iob_blkno = curriob->blkno;
    iob_bcount = curriob->bcount;
    if(valid_flag == NORMAL_LRU) temp_lpn = curriob->blkno/SECT_NUM_PER_PAGE;
    else temp_lpn  = com_lpn;
    	
    if(iob_blkno%SECT_NUM_PER_PAGE != 0)
    {
     printf("blkno is not aligned for FtlGetIobOverhead\n");
     exit(1); 
    }
    if(iob_bcount%SECT_NUM_PER_PAGE != 0)
    {
     printf("bcount includs partial page for FtlGetIobOverhead\n");
     exit(1); 
    }
    
    switch(cache_policy){
    	case PPC: case CHUNKPPC:
    		raid_read_overhead = GetRaidReadOverhead(temp_lpn,valid_flag); break;
    	default:break;
    }
    if((curriob->flags & READ) == WRITE)//write
    	{
      ftlaccoverhead = FtlCplReq(iob_blkno,(curriob->flags & READ),curriob); 
      curriob->availtime = ftlaccoverhead + simtime;
     }
     else//read
     {
      if(curriob->bufcpltime > simtime )
       {
        ftlaccoverhead = curriob->bufcpltime - simtime;
        curriob->availtime = curriob->bufcpltime;
       }
      else
       {
        curriob->availtime = simtime;   	
       }
     }    
    
    curriob->state = FTL_BUF_CPL;  
    
    FtlRemovedLRUCplIob(curriob);
    switch(cache_policy){
    	case PPC: case CHUNKPPC:
    		UpdateEntryInPPCTable(curriob->blkno/SECT_NUM_PER_PAGE,0,RemoveEntry); break;
    	default:break;
    }
  }		
 ftlaccoverhead = max(raid_read_overhead,ftlaccoverhead);
 return(ftlaccoverhead);
}

double FtlUpdateIobState(ioreq_event *curr, double blktranstime)
{ 
 fltiob *curriob;
 int i;
 double time;
 
 time = blktranstime;
 
 for(i=0;i<FtlBuf->CwqPointer; i++)
 {
 	curriob = FtlBuf->CurrWaitQueue[i];

 	if(curriob->curr_flag != 1)
  {
   printf("Error: curr_flag is not correct for FtlUpdateIobState\n");
   exit(1);
  }  	
 	curriob->curr_flag = 0;
 	if(curriob->flags != curr->flags)
  {
   printf("Error: curr->flags is not correct for FtlUpdateIobState\n");
   exit(1);
  } 
  if((curriob->flags & READ) == 0)//write
   {
   	if(curriob->state != FTL_BUF_WAIT_DATA)
     {
      printf("Error: curr->state is not correct for FtlUpdateIobState\n");
      exit(1);
     } 
    curriob->state = FTL_BUF_DATA_ARRIVE;
   }
  else//read
   {
    if(curriob->state != FTL_BUF_DATA_ARRIVE)
     {
      printf("Error: curr->state is not correct for FtlUpdateIobState\n");
      exit(1);
     }
    curriob->state = FTL_BUF_TX_CPL;
   }	
    curriob->bufcpltime = curriob->arrivetime+time;
    curriob->starttime = curriob->bufcpltime; 
    time = time+blktranstime;
 }
 
// if(FtlBuf->CwqPointer != (curr->bcount/SECT_NUM_PER_PAGE))
//  {
//   printf("Error: FtlBuf->CwqPointer is not correct for FtlUpdateIobState\n");
//   exit(1); 	
//  }
 FtlBuf->CwqPointer = 0; 
 return(time);
}

int FltGetOutstandingIobNum(unsigned int bcount)
{
	if(bcount > FTL_BUF_NUM)
  {
   printf("Error: bcount exceeds maximum buffer number for FltGetOutstandingIobNum\n");
   exit(1);    	
  }		
  
  if(FtlBuf->OutstandingIobNum != (FtlBuf->UsedBufSize/SECT_NUM_PER_PAGE))
  {
   printf("Error: OutstandingIobNum is inconsitent with UsedBufSize for FltGetOutstandingIobNum\n");
   exit(1);    	
  }	
 return(FtlBuf->OutstandingIobNum);	
}

fltiob * FtlChkBufHit(unsigned int blkno)
{

 fltiob * temp = NULL;
 fltiob * temp1 = NULL;
  
 int i;	
 
 if(FtlBuf->OutstandingIobNum == 0)
	{ 
	 if(FtlBuf->UsedIob_head != NULL)
   {
    printf("No outstanding iob but UsedIob_head is not NULL for FtlChkBufHit\n");
    exit(1);   	
   }
  }
 else
   {
    temp = FtlBuf->UsedIob_head;
 
    for(i=0;i<(FtlBuf->OutstandingIobNum);i++)
    {
     if(temp->state != FTL_BUF_DATA_ARRIVE && (temp->flags & READ) == 0 && temp->curr_flag == 0)//write
     {
      printf("temp->state is not FTL_BUF_DATA_ARRIVE for FtlChkBufHit\n");
      exit(1);      	
     }
     if(temp->state != FTL_BUF_TX_CPL && (temp->flags & READ) == 1 && temp->curr_flag == 0)//read
     {
      printf("temp->state is not FTL_BUF_TX_CPL for FtlChkBufHit\n");
      exit(1);      	
     }
     if(temp->starttime < simtime)//start flush time
     {
     	temp->starttime = simtime;
     }            
     if(temp->blkno == blkno && temp->curr_flag == 0)//hit
     {
      temp1 = temp;      	
     }
     if(i == (FtlBuf->OutstandingIobNum-1) && temp != FtlBuf->UsedIob_tail)//the last buf
     {
      printf("OutstandingIobNum is not correct for FtlChkBufHit\n");
      exit(1);       
     }
     temp = temp->next;             
    }   	   
   } 
 return(temp1);
}

/**************function for DDA
*********************************/

unsigned int ConvDDALpn(unsigned int index_no,int strip_no, int type)
{
	unsigned int conv_lpn;
	
	switch(type){
		case INDEX_LPN:
			conv_lpn = index_no*page_per_strip+strip_no+visible_blkno/SECT_NUM_PER_PAGE; break;
		case INDEX_PARITY_LPN:
			conv_lpn = index_no+visible_blkno/SECT_NUM_PER_PAGE/page_per_strip + parity_blkno/SECT_NUM_PER_PAGE; break;	
	  default: break;}
	return (conv_lpn);	
}

int UpdateIndexEntry(sie_t *currindex,int strip_no, unsigned int lpn, int type)
{
 
 int flag = 0;
 int i,j;
 sie_t *tempindex;
 
 switch(type){
   case AddEntry:
   	 if(currindex->wr_strip_num > page_per_strip)
 	    {printf("currindex->wr_strip_num is not correct in UpdateIndexEntry!!\n"); exit(-1);}

   	 currindex->lpn[strip_no] = lpn;
   	 currindex->vld[strip_no] = 1;
   	 currindex->wr_strip_num++; break;
   case CompEntry:
   	 tempindex = dda_strip_table->UsedHead;
   	 for(i=0;i<dda_strip_table->TotalNum;i++){
   	 	 for(j=0;j<tempindex->wr_strip_num;j++){
   	   if(tempindex->lpn[j] == currindex->lpn[strip_no] && tempindex->vld[j] == 1 &&
   	   	  tempindex->acctime < currindex->acctime){
      	tempindex->vld[j] == 0;}
   	   if(tempindex->lpn[j] == currindex->lpn[strip_no] && tempindex->vld[j] == 1 &&
   	   	  tempindex->acctime > currindex->acctime){
      	flag == 1;}      	   }
       tempindex = tempindex->next;} 
   	 if(tempindex!= NULL)
 	    {printf("dda_strip_table->UsedTail is not correct in UpdateIndexEntry(CompEntry)!!\n"); exit(-1);}      
     break;   
    case InvalidEntry:
   	 tempindex = dda_strip_table->UsedHead;
   	 for(i=0;i<dda_strip_table->TotalNum;i++){
   	 	 for(j=0;j<tempindex->wr_strip_num;j++){
   	   if(tempindex->lpn[j] == lpn && tempindex->vld[j] == 1){
         if(tempindex->lpn[j] == lpn)tempindex->vld[j] == 0;}}
       tempindex = tempindex->next;} 
   	 if(tempindex != NULL)
 	    {printf("dda_strip_table->UsedTail is not correct in UpdateIndexEntry(InvalidEntry)!!\n"); exit(-1);}      
     break;
    case AddIndex:{
    	if(dda_strip_table->UsedHead == NULL){
         dda_strip_table->UsedTail = currindex; dda_strip_table->UsedHead = currindex;
         currindex->prev = NULL; currindex->next = NULL;}
      else{
      	 dda_strip_table->UsedTail->next = currindex; currindex->prev = dda_strip_table->UsedTail;
      	 dda_strip_table->UsedTail = currindex; currindex->next = NULL;}
      	 dda_strip_table->TotalNum++; break;}		
   default:break;
 }
 return(flag);
}

double PlainRaidOpt(unsigned int rest_strip_start_no,int rest_page_num, int flag)
{
	double delay = 0;
	double tempdelay = 0;
	unsigned int curr_strip_start_no;
  int curr_strip_page_num;
  int j;
  	
  while(rest_page_num){
    if(flag == WRITE){//0: write
      //determine strip
      curr_strip_start_no = (rest_strip_start_no/page_per_strip)*page_per_strip;
      if( rest_page_num >= (page_per_strip - rest_strip_start_no%page_per_strip))
        curr_strip_page_num = page_per_strip - (rest_strip_start_no%page_per_strip);
      else if( rest_page_num < (page_per_strip - rest_strip_start_no%page_per_strip))
      	curr_strip_page_num = rest_page_num;
      
      if(curr_strip_page_num == page_per_strip)
      	{
      	 for(j = 0; j < page_per_strip; j++){
           tempdelay = FtlCplReq(curr_strip_start_no*SECT_NUM_PER_PAGE,flag,0);//write
           delay = max(tempdelay,delay);
           curr_strip_start_no = curr_strip_start_no + 1;
         }
      	} 
      else if(curr_strip_page_num > page_per_strip/2 )
      	{
      	 for(j = 0; j < page_per_strip; j++){
      	  if(curr_strip_start_no<rest_strip_start_no || 
      	  	 curr_strip_start_no>=(rest_strip_start_no + curr_strip_page_num))
           tempdelay = FtlCplReq(curr_strip_start_no*SECT_NUM_PER_PAGE,1,0);//read
          else 
           tempdelay = FtlCplReq(curr_strip_start_no*SECT_NUM_PER_PAGE,flag,0);//write
          delay = max(tempdelay,delay);
          curr_strip_start_no = curr_strip_start_no + 1;          
      	 }
        } 
      else// if(curr_strip_page_num <= page_per_strip/2)	
        {
         curr_strip_start_no = rest_strip_start_no;
      	 for(j = 0; j < curr_strip_page_num; j++){
           tempdelay = FtlCplReq(curr_strip_start_no*SECT_NUM_PER_PAGE,1,0);//read
           delay = max(tempdelay,delay);
           tempdelay = FtlCplReq(curr_strip_start_no*SECT_NUM_PER_PAGE,flag,0);//write
           delay = max(tempdelay,delay);
           curr_strip_start_no = curr_strip_start_no + 1;          
      	   } 
         curr_strip_start_no = (rest_strip_start_no/page_per_strip)+ parity_blkno/SECT_NUM_PER_PAGE;
     	   tempdelay = FtlCplReq(curr_strip_start_no*SECT_NUM_PER_PAGE,1,0);//read
         delay = max(tempdelay,delay);      	           	
        }
      curr_strip_start_no = (rest_strip_start_no/page_per_strip)+ parity_blkno/SECT_NUM_PER_PAGE;
     	tempdelay = FtlCplReq(curr_strip_start_no*SECT_NUM_PER_PAGE,WRITE,0);//write
      delay = max(tempdelay,delay);

      rest_page_num = rest_page_num - curr_strip_page_num;
      rest_strip_start_no = rest_strip_start_no + curr_strip_page_num;      
      }
    else{//1:read
      tempdelay = FtlCplReq(rest_strip_start_no*SECT_NUM_PER_PAGE,flag,0);
      delay = max(tempdelay,delay);
      rest_strip_start_no = rest_strip_start_no + 1;
      rest_page_num = rest_page_num - 1;
     }
  }
   return(delay);
}

sie_t * GetLRUIndex(int type)
{
 sie_t * tempindex;
 sie_t * tempindex1 = NULL;
 double acctime = 0;
 int i,j,cnt; 
 int total_cnt = 0;
 
 tempindex = dda_strip_table->UsedHead;
 tempindex1 = tempindex;
 acctime = tempindex->acctime;
 
 switch(type){
 	 case NORMAL_LRU:
     for(i=0;i<dda_strip_table->TotalNum;i++)
      {if(tempindex->acctime < acctime)
  	     {acctime = tempindex->acctime; tempindex1 = tempindex;}
          tempindex = tempindex->next;
     }break;
   case NORMAL_CNT:
     for(i=0;i<dda_strip_table->TotalNum;i++)
      { cnt = 0;
      	for(j=0;j<tempindex->wr_strip_num;j++)
  	     {if(tempindex->vld[j] == 1) cnt++;}
  	    if(total_cnt == 0 || 
  	    	(total_cnt > cnt && total_cnt != 0)) 
  	    	{tempindex1 = tempindex; total_cnt = cnt;}
        tempindex = tempindex->next;
     }break;   	
 } 
 if(tempindex != NULL)
  {printf("dda_strip_table->UsedTail and dda_strip_table->TotalNum are not consistent in GetLRUIndex!!\n"); exit(-1);}
  
 if(dda_strip_table->UsedTail == dda_strip_table->UsedHead)  
 	 {dda_strip_table->UsedHead = NULL; dda_strip_table->UsedTail = NULL;}
 else if(tempindex1 == dda_strip_table->UsedHead)
 	 {dda_strip_table->UsedHead = tempindex1->next; dda_strip_table->UsedHead->prev = NULL;}
 else if(tempindex1 == dda_strip_table->UsedTail)
 	 {dda_strip_table->UsedTail = tempindex1->prev; dda_strip_table->UsedTail->next = NULL;}
 else
 	 {tempindex1->next->prev = tempindex1->prev; tempindex1->prev->next = tempindex1->next;}
 dda_strip_table->TotalNum--;	 
 return(tempindex1);
}

double ClearIndexEntry(sie_t * currindex)
{
 fltiob * tempiob;
 double overhead = 0;
 unsigned int curr_lpn;
 int i;
 int flag = 0;//1: found more recent io access 
 
 for(i=0;i<page_per_strip;i++){
 	 tempiob = FtlChkBufHit(currindex->lpn[i]*SECT_NUM_PER_PAGE);
 	 if(currindex->vld[i]){//invalidate all other lpn
     flag = UpdateIndexEntry(currindex,i,currindex->lpn[i],CompEntry);}
   if(tempiob == NULL && currindex->vld[i] == 1 && flag == 0){//not in iob or no other more recent access
   	 curr_lpn = ConvDDALpn(currindex->index_no,i,INDEX_LPN);
   	 overhead = FtlCplReq(curr_lpn*SECT_NUM_PER_PAGE,READ,NULL);
   	 overhead = max(overhead,PlainRaidOpt(currindex->lpn[i], SECT_NUM_PER_PAGE, WRITE));}
   currindex->vld[i] = 0;
  }
  
  currindex->wr_strip_num = 0;
  currindex->acctime = 0;
  currindex->next = NULL;
  currindex->prev = NULL;
  
  if(dda_strip_table->AvailHead == NULL && dda_strip_table->AvailTail != NULL)
  {printf("dda_strip_table->AvailHead and dda_strip_table->AvailTail are not consistent in ClearIndexEntry!!\n"); exit(-1);}
  	
 if(dda_strip_table->AvailHead == NULL)  
 	 {dda_strip_table->AvailHead = currindex; dda_strip_table->AvailTail = currindex;}
 else
 	 {dda_strip_table->AvailTail->next = currindex; dda_strip_table->AvailTail = currindex;}

 return(overhead);
}

sie_t * GetNewIndex()
{
 sie_t * tempindex;
 int i;

 if(dda_strip_table->AvailHead == NULL)
 {printf("dda_strip_table->AvailHead is not correct in GetNewIndex!!\n"); exit(-1);}
 tempindex = dda_strip_table->AvailHead;  
 if(dda_strip_table->AvailTail == dda_strip_table->AvailHead)  
 	 {dda_strip_table->AvailHead = NULL; dda_strip_table->AvailTail = NULL;}
 else
 	 {dda_strip_table->AvailHead = tempindex->next; dda_strip_table->AvailHead->prev = NULL;}
 	 
 return(tempindex);
}

sie_t * FtlGetNewIndex()
{
 sie_t * tempindex;
 sie_t * currindex;
 
 if(dda_strip_table->UsedHead == NULL || dda_strip_table->TotalNum == 0)
  {printf("No available index or used index in FtlGetNewIndex!!\n"); exit(-1);}
 	
 currindex = GetLRUIndex(NORMAL_CNT);
 raid_evict_overhead = ClearIndexEntry(currindex);
 tempindex = GetNewIndex();
 return(tempindex); 	
}

sie_t * FtlGetLRUIndex()
{
 sie_t * tempindex;
 
 if(dda_strip_table->AvailHead == NULL && dda_strip_table->AvailTail != NULL)
  {printf("dda_strip_table->AvailHead and dda_strip_table->AvailTail are not consistent in FtlGetLRUIndex!!\n"); exit(-1);}

 if(dda_strip_table->AvailHead != NULL){ 
 	  tempindex = dda_strip_table->AvailHead;
 	  if(dda_strip_table->AvailHead == dda_strip_table->AvailTail)
 	  	{dda_strip_table->AvailHead = NULL; dda_strip_table->AvailTail = NULL;}
 	  else
 	  	{dda_strip_table->AvailHead = dda_strip_table->AvailHead->next; dda_strip_table->AvailHead->prev = NULL;}
  } 
 else{
 	   tempindex = FtlGetNewIndex();
  }
 return(tempindex);	 	  	
}

int ChkPPCTableHit(unsigned int lpn)
{
	unsigned int parity_lpn;
	entry_t * temp1 = NULL;
	entry_t * temp;
	int flag = 0;
	
  if(lpn>=parity_blkno/SECT_NUM_PER_PAGE) parity_lpn = lpn;  
  else parity_lpn = lpn/page_per_strip + parity_blkno/SECT_NUM_PER_PAGE;
  	
  if(ppc_table->UsedHead == NULL && ppc_table->UsedTail != NULL)
 	  {printf("ppc_table->UsedTail and ppc_table->UsedHead are not consistent in ChkPPCTableHit!!\n"); exit(-1);} 	        	        
  temp = ppc_table->UsedHead;
  while(temp){
    if(temp->ParityLPN == parity_lpn && temp1 == NULL) {temp1 = temp; flag = 1;}
    else if(temp->ParityLPN == parity_lpn && temp1 != NULL)
    	{printf("Multiple cache entries record the same parity lpn in ChkPPCTableHit!!\n"); exit(-1);}
    temp = temp->next;   	
    }
  return(flag);
}

double GetEvictIndexOverhead(sie_t *currindex,int type)
{	
 fltiob * curriob;
 entry_t * currentry;
 int i;
 double ftlaccoverhead = 0;
 double tempoverhead = 0; 
 unsigned int iob_blkno;
 unsigned int curr_lpn;
 int iob_bcount;     
	
   for(i=0;i<page_per_strip;i++){//select an entry for eviction
     currentry = FtlGetStrip(NORMAL_LRU_CNT);
     if(currentry == NULL){printf("currentry is NOT correct in GetEvictIndexOverhead!!\n"); exit(-1);}     
     curriob = FtlChkBufHit(currentry->ModifiedLPN[0]*SECT_NUM_PER_PAGE);          
     if(curriob == NULL){printf("curriob is NOT correct in GetEvictIndexOverhead!!\n"); exit(-1);}
     if(curriob->curr_flag == 1){printf("Error: curr_flag is not correct for GetEvictIndexOverhead\n"); exit(-1);}         
   //3: get blk info
    iob_blkno = curriob->blkno;
    iob_bcount = curriob->bcount;
    if(curriob->blkno/SECT_NUM_PER_PAGE!=currentry->ModifiedLPN[0]){printf("Error: curriob->blkno is not same with SECT_NUM_PER_PAGE for GetEvictIndexOverhead\n"); exit(-1);}
    	
    if(iob_blkno%SECT_NUM_PER_PAGE != 0)
    {
     printf("blkno is not aligned for GetEvictIndexOverhead\n");
     exit(1); 
    }
    if(iob_bcount%SECT_NUM_PER_PAGE != 0)
    {
     printf("bcount includs partial page for GetEvictIndexOverhead\n");
     exit(1); 
    }
    curr_lpn = ConvDDALpn(currindex->index_no,i,INDEX_LPN);//convert into index lpn

     if(ChkPPCTableHit(iob_blkno/SECT_NUM_PER_PAGE) == 0){
       printf("one strip hit in GetEvictIndexOverhead\n"); exit(-1);}
   
    if((curriob->flags & READ) == WRITE){
      tempoverhead = FtlCplReq(curr_lpn*SECT_NUM_PER_PAGE,(curriob->flags & READ),curriob); 
      curriob->availtime = ftlaccoverhead + simtime;
      ftlaccoverhead = max(tempoverhead,ftlaccoverhead); 
     }
     else{//read
      if(curriob->bufcpltime > simtime ){
        ftlaccoverhead = curriob->bufcpltime - simtime;
        curriob->availtime = curriob->bufcpltime;}
      else{
        curriob->availtime = simtime;}}    
    
    curriob->state = FTL_BUF_CPL;  
    
    UpdateIndexEntry(currindex,i,curriob->blkno/SECT_NUM_PER_PAGE,AddEntry);     	          
    FtlRemovedLRUCplIob(curriob);
    if(type == 1){//HDA
    UpdateEntryInPPCTable(currentry->ModifiedLPN[0],0,RemoveEntry);}          
  }  
  return(ftlaccoverhead);
}

double FtlGetDDAIobOverhead(int valid_flag,unsigned int com_lpn)
{
 fltiob * curriob;
 fltiob * tempiob;
 sie_t  * currindex;
 int i;
 double ftlaccoverhead = 0;
 double tempoverhead = 0; 
 unsigned int iob_blkno;
 unsigned int curr_lpn;
 int iob_bcount;     

   //1: get index
   switch(valid_flag){
   	case GETINDEX: currindex = FtlGetLRUIndex();
   	ftlaccoverhead = raid_evict_overhead; break;
   	default: break; }
   
   //2: get iorequest
   for(i=0;i<page_per_strip;i++){//select an entry for eviction
     curriob = FtlGetLRUPendingIob(); 
     if(curriob == NULL){printf("curriob is NOT correct in FtlGetDDAIobOverhead!!\n"); exit(-1);}
     if(curriob->curr_flag == 1){printf("Error: curr_flag is not correct for FtlGetDDAIobOverhead\n"); exit(-1);}         
   //3: get blk info
    iob_blkno = curriob->blkno;
    iob_bcount = curriob->bcount;
    	
    if(iob_blkno%SECT_NUM_PER_PAGE != 0)
    {
     printf("blkno is not aligned for FtlGetDDAIobOverhead\n");
     exit(1); 
    }
    if(iob_bcount%SECT_NUM_PER_PAGE != 0)
    {
     printf("bcount includs partial page for FtlGetDDAIobOverhead\n");
     exit(1); 
    }
    curr_lpn = ConvDDALpn(currindex->index_no,i,INDEX_LPN);//convert into index lpn
   
    if((curriob->flags & READ) == WRITE){
      tempoverhead = FtlCplReq(curr_lpn*SECT_NUM_PER_PAGE,(curriob->flags & READ),curriob); 
      curriob->availtime = ftlaccoverhead + simtime;
      ftlaccoverhead = max(tempoverhead,ftlaccoverhead); 
     }
     else{//read
      if(curriob->bufcpltime > simtime ){
        ftlaccoverhead = curriob->bufcpltime - simtime;
        curriob->availtime = curriob->bufcpltime;}
      else{
        curriob->availtime = simtime;}}    
    
    curriob->state = FTL_BUF_CPL;  
    
    UpdateIndexEntry(currindex,i,curriob->blkno/SECT_NUM_PER_PAGE,AddEntry);//modify need to invalidate
    FtlRemovedLRUCplIob(curriob);     
  }

 //4:write parity
 UpdateIndexEntry(currindex,0,0,AddIndex); 
 curr_lpn = ConvDDALpn(currindex->index_no,0,INDEX_PARITY_LPN);
 tempoverhead = FtlCplReq(curr_lpn*SECT_NUM_PER_PAGE,WRITE,0);
 ftlaccoverhead = max(tempoverhead,ftlaccoverhead);
 
 return(ftlaccoverhead);
}
/******************function for dda end
********************/
/******************fuction for hda start
********************/
entry_t * FtlGetStrip(int type)
{
 entry_t * temp;
 entry_t * temp1 = NULL;
 int len = 0;
 int cnt = 0;
 double acctime;
 
 if(ppc_table->UsedHead == NULL)
   {printf("remove parity from the empty in FtlGetStrip!!\n"); exit(-1);}
 if(ppc_table->UsedHead == NULL && ppc_table->UsedTail != NULL)
   {printf("ppc_table->UsedTail and ppc_table->UsedHead are not consistent in FtlGetStrip!!\n"); exit(-1);} 	        
 if(ppc_table->TotalNum == 0)
   {printf("TotalNum is NOT correct in FtlGetStrip!!\n"); exit(-1);} 
   	        
 switch(type){
 	case NORMAL_LEN:{
    temp = ppc_table->UsedHead;
    temp1 = temp;
    len = temp1->TotalOldPBN; acctime = temp1->acctime; 		
    while(temp){
    if(temp->TotalOldPBN > len ||
   	  (temp->TotalOldPBN == len && temp->acctime < acctime))
      {temp1 = temp; acctime = temp->acctime; len = temp->TotalOldPBN;}
     temp = temp->next; }
 	 if(temp1->TotalOldPBN == 0)
 		 {printf("TotalOldPBN is equal to 0 in FtlGetStrip\n"); exit(-1);}
 	 return(temp1);
 	 break;}
 	case NORMAL_CNT:{
    temp = ppc_table->UsedHead;
    if(temp->TotalOldPBN == 1 && temp->acctime != simtime){temp1 = temp; acctime = temp1->acctime; cnt = temp1->AccCnt; len = 1;}
    else{temp1 = NULL; acctime = 0;len = 0; cnt = 0;} 
    	 		
    while(temp){
    if(temp->TotalOldPBN == 1 && temp->acctime != simtime){
    	if(temp1 == NULL ||
    		(temp->AccCnt > cnt && temp1 != NULL) ||
    		(temp->AccCnt == cnt && temp->acctime < acctime && temp1 != NULL))
    		{temp1 = temp; acctime = temp->acctime; cnt = temp->AccCnt;}
        len++;}
      temp = temp->next; }
 	 if(len >= page_per_strip)return(temp1);
 	 else return(NULL);
 	 break;} 	 
 	case NORMAL_LRU_CNT:{
    temp = ppc_table->UsedHead;
    if(temp->TotalOldPBN == 1 && temp->acctime != simtime){temp1 = temp; acctime = temp1->acctime; cnt = temp1->AccCnt; len = 1;}
    else{temp1 = NULL; acctime = 0;len = 0; cnt = 0;} 
    	 		
    while(temp){
    if(temp->TotalOldPBN == 1 && temp->acctime != simtime){
    	if(temp1 == NULL ||
    		(temp->AccCnt > cnt && temp1 != NULL) ||
    		(temp->AccCnt == cnt && temp->acctime < acctime && temp1 != NULL))
    		{temp1 = temp; acctime = temp->acctime; cnt = temp->AccCnt;}
        len++;}
      temp = temp->next; }
 	 return(temp1);
 	 break;}  	 
 	case NORMAL_LRU:{
    temp = ppc_table->UsedHead;
    if(temp->TotalOldPBN == 1 && temp->acctime != simtime){temp1 = temp; acctime = temp1->acctime;len = 1;}
    else{temp1 = NULL; acctime = 0;len = 0;} 
    	 		
    while(temp){
    if(temp->TotalOldPBN == 1 && temp->acctime != simtime){
    	if(temp1 == NULL ||
    		(temp->acctime < acctime && temp1 != NULL))
    		{temp1 = temp; acctime = temp->acctime;}
        len++;}
      temp = temp->next; }
 	  return(temp1);
 	 break;} 	 
  default:break;}	
}

//sie_t * FtlGetIndex(int type)
//{
// sie_t * tempindex;
// sie_t * tempindex1 = NULL;
// double acctime = 0;
// int i;
// int strip_cnt = 0;
// int strip_page;
// 
// tempindex = dda_strip_table->UsedHead;
// if(tempindex != NULL){
// if(ChkStripNum(tempindex->lpn[0]) == 1) tempindex1 = tempindex;
//  acctime = tempindex->acctime;
// 
// for(i=0;i<dda_strip_table->TotalNum;i++)
//  {
//   strip_page = ChkStripNum(tempindex->lpn[0]);
//   if(tempindex->acctime < acctime && strip_page == 1)
//  	{acctime = tempindex->acctime; tempindex1 = tempindex;}
//   if(strip_page == 1) strip_cnt++;
//   tempindex = tempindex->next;
//  }
// if(tempindex != NULL)
//  {printf("dda_strip_table->UsedTail and dda_strip_table->TotalNum are not consistent in GetLRUIndex!!\n"); exit(-1);}
// }
// if(strip_cnt < page_per_strip) return(NULL);
// else return(tempindex1);
//}

int ChkEvictMode()
{
 entry_t  * currindex;
 entry_t  * currentry;

 currindex = FtlGetStrip(NORMAL_CNT);//if null: not enough small io, if not, return the most recent one
 currentry = FtlGetStrip(NORMAL_LEN);
 
// if(currindex == NULL ||
// 	 (currentry->acctime < currindex->acctime && currentry->TotalOldPBN >= 2))
 if(currindex == NULL ||
 	 (currentry->acctime < currindex->acctime || currindex->AccCnt < 4)) 	 
  return(0);
 else
 	return(1);
}

double GetEvictStripOvhead( entry_t * currentry)
{
	fltiob * curriob;
	double read_overhead = 0;
	int modified_page_number = 0;
	unsigned int curr_read_lpn;
	unsigned int curr_read_ppn;
  unsigned int return_val;
	int i;

	curr_read_lpn = currentry->ParityLPN;
	if(currentry->TotalOldPBN == 0){printf("TotalOldPBN is equal to 0 in GetEvictStripOvhead\n"); exit(-1);}

  if(ChkPPCTableHit(curr_read_lpn) == 0){
    printf("no strip hit in GetEvictStripOvhead\n"); exit(-1);}
		
	modified_page_number = ChkModPageNum(curr_read_lpn);
  curr_read_lpn = (curr_read_lpn - parity_blkno/SECT_NUM_PER_PAGE)*page_per_strip;//1st strip page
		
     for(i=0;i<page_per_strip;i++){
  	   return_val = ChkPPCOldPPN(curr_read_lpn);//return_val = curr_read_ppn << 1 | hit flag
  	   curr_read_ppn = return_val>>1; return_val = return_val & 0x1;
  	   
	     if(modified_page_number >= page_per_strip/2){//read unmodified data   	   
  	      if(return_val == 0)
	          read_overhead = max(read_overhead,FtlCplReq(curr_read_lpn*SECT_NUM_PER_PAGE,READ,0));} 
	     else{
  	      if(return_val == 1)
	          read_overhead = max(read_overhead,FtlCplReq(curr_read_lpn*SECT_NUM_PER_PAGE,READ,0));} 
	     if(return_val == 1){ 
  	      curriob = FtlChkBufHit(curr_read_lpn*SECT_NUM_PER_PAGE);
  	      if(curriob == NULL){printf("curriob is not correct in GetEvictStripOvhead!!\n"); exit(-1);}
          read_overhead = max(FtlCplReq(curr_read_lpn*SECT_NUM_PER_PAGE,WRITE,curriob),read_overhead); 
          curriob->availtime = read_overhead + simtime;
          curriob->state = FTL_BUF_CPL; 
          FtlRemovedLRUCplIob(curriob);} 
       curr_read_lpn++;}
       
     UpdateEntryInPPCTable(currentry->ParityLPN,0,RemoveEntry);      	          
	   curr_read_lpn = (curr_read_lpn-1)/page_per_strip + parity_blkno/SECT_NUM_PER_PAGE; 
	 
	   if(modified_page_number < page_per_strip/2)
		  {read_overhead = max(read_overhead,FtlCplReq(curr_read_lpn*SECT_NUM_PER_PAGE,READ,0));}
	return(read_overhead);
}

double FtlGetHDAIobOverhead(int valid_flag,unsigned int com_lpn)
{
 sie_t  * currindex;
 entry_t * currentry;
 int i,evict_mode;
 double ftlaccoverhead = 0;
 double tempoverhead = 0; 
 unsigned int curr_lpn;  

   //0:get evict mode
   evict_mode = ChkEvictMode();
   
   if(evict_mode == 1){//raid-index evict
   	 currindex = FtlGetLRUIndex();
   	 if(currindex->wr_strip_num != 0){printf("currindex->wr_strip_num is not correct in FtlGetHDAIobOverhead!!\n"); exit(-1);}
   	 tempoverhead = GetEvictIndexOverhead(currindex,1);
   	 ftlaccoverhead = max(tempoverhead,raid_evict_overhead);     
     UpdateIndexEntry(currindex,0,0,AddIndex); 
     curr_lpn = ConvDDALpn(currindex->index_no,0,INDEX_PARITY_LPN);
     ftlaccoverhead = max(ftlaccoverhead,FtlCplReq(curr_lpn*SECT_NUM_PER_PAGE,WRITE,0));
   	 }
   else{//raid evict
     currentry = FtlGetStrip(NORMAL_LEN);
     
     if(ChkPPCTableHit(currentry->ModifiedLPN[0]) == 0){
       printf("no strip hit after FtlGetStrip in FtlGetHDAIobOverhead!!\n"); exit(-1);}      
     ftlaccoverhead = max(ftlaccoverhead,GetRaidReadOverhead(currentry->ParityLPN,NORMAL_LRU));
     if(ChkPPCTableHit(currentry->ModifiedLPN[0]) == 0){
       printf("no strip hit in FtlGetHDAIobOverhead\n"); exit(-1);}  
     tempoverhead = GetEvictStripOvhead(currentry);   
     ftlaccoverhead = max(ftlaccoverhead,tempoverhead); 
     ftlaccoverhead = max(ftlaccoverhead,FtlCplReq((currentry->ParityLPN*SECT_NUM_PER_PAGE),WRITE,0));
     };	
 UpdateCnt(evict_mode);
 return(ftlaccoverhead);
}
/******************fuction for hda end
********************/
double FtlGetNewIobOverhead(unsigned int blkno,int flags)
{
 fltiob * temp;
 double overhead = 0;
 double rdacctime = 0;
    
 //check whether enough buffer is available
 if(FtlChkIobfnum(SECT_NUM_PER_PAGE) == 0)//no enough buffer
  {
   switch(cache_policy){
      case LRU: 
      case PPC:
      case CHUNKPPC:      	
       overhead = FtlGetIobOverhead(NORMAL_LRU,0);break;
      case DDA:   
       overhead = FtlGetDDAIobOverhead(GETINDEX,0);break;
      case HDA: 
       overhead = FtlGetHDAIobOverhead(GETINDEX,0);break;      	
      default:break;	
  }}

 if(FtlChkIobfnum(SECT_NUM_PER_PAGE) == 0)//no enough buffer
  {
  printf("one buffer has been released, but check result is not correct for FtlGetNewIobOverhead\n");
  exit(1);   
  }  
 
 temp = FtlAddNewUsedIob(blkno,flags);
 
 if((flags & READ) == 1)//read
 {
 	 rdacctime = FtlCplReq(blkno,flags,temp);
   overhead = max(overhead,rdacctime);  	
 }
  return(overhead); 
}

double FtlUpdateHitBufOverhead(unsigned int blkno, int flags, fltiob *iobuf)
{
 
 double overhead = 0;
    
 if((iobuf->flags & READ) == 1 && iobuf->state != FTL_BUF_TX_CPL)//read	
 {
  printf("iobuf->state is not correct for FtlUpdateHitBufOverhead\n");
  exit(1);     
 }
 if((iobuf->flags & READ) == 0 && iobuf->state != FTL_BUF_DATA_ARRIVE)//write	
 {
  printf("iobuf->state is not correct for FtlUpdateHitBufOverhead\n");
  exit(1);     
 }
 iobuf->flags = flags;
     
 if(iobuf->curr_flag != 0)
 {
  printf("curr_flag is not correct for FtlUpdateHitBufOverhead\n");
  exit(1);
 }
 switch(cache_policy){
 	case LRU: case DDA: case HDA:
    iobuf->curr_flag = 1;break;
  default: break;
  }
  
 if(simtime >= iobuf->starttime)
 	{
   iobuf->starttime  = simtime;
  }
 else
 	{
 	 overhead = iobuf->starttime - simtime;	
 	}
 if(simtime >= iobuf->bufcpltime)
 	{
   iobuf->arrivetime  = simtime;
   iobuf->bufcpltime  = simtime;
  }
  else
  {  
   iobuf->arrivetime = iobuf->bufcpltime;//io arrive must wait for previous io cpl
  } 
  
  switch(cache_policy){
  	case LRU:
  	case DDA:
  	case HDA:{
      if((flags & READ) == 0)//write
        {
         iobuf->state = FTL_BUF_WAIT_DATA;	
        }
      else
        {
         iobuf->state = FTL_BUF_DATA_ARRIVE;		
        }
      FtlBuf->CurrWaitQueue[FtlBuf->CwqPointer] = iobuf;
      FtlBuf->CwqPointer++;break;}
    case PPC:
    case CHUNKPPC:{
      iobuf->state = FTL_BUF_DATA_ARRIVE;break;}
    default: break;
   }
  return(overhead); 
}

int FtlChkThreshold()
{
 if(FtlBuf->OutstandingIobNum > FTL_BUF_THRU)
 	return(1);
 else
 	return(0);
}

void warmFlash(char *tname){

  FILE *fp = fopen(tname, "r");
  char buffer[80];
  double time;
  int devno, bcount, flags;
  unsigned int blkno = 0;
  double delay;
  unsigned int i;
  unsigned int cenum = 0;
  bcount = SECT_NUM_PER_PAGE;
  
  while(blkno < (maxblk-SECT_NUM_PER_PAGE)){
          	
    delay = callFsim(blkno, bcount, 0, cenum); 

    blkno += SECT_NUM_PER_PAGE;
    //for(i = blkno; i<(blkno+bcount); i++){ dm_table[i] = DEV_FLASH; }
  }
  printf("initialization is finished\n");
  nand_stat_reset();

  fclose(fp);
}


void FtlStatUpdate(fltiob *iobuf,unsigned int cnt)
{
  FtlStat->TotalLpNum++;
  FtlStat->Total_Simtime = simtime;
  if(cnt == 0)
  {
   FtlStat->Total_IoReq++;	
  }    
  if(iobuf != NULL)//hit
 	 {
    FtlStat->TotalHitNum++;
    FtlStat->Total_Dramaccess_energy += (FTLDR_DYN_PC);
   }
  else
   {
   	FtlStat->Total_Dramaccess_energy += (FTLDR_DYN_PC*2);
   } 	 
}

void FtlPrintStat(FILE *outFP)
{
 double hitrate = 0;
 double ave_pc = 0; 
 double pc_per_page = 0;
 double peak_pc = 0;
 
 hitrate = ((double)FtlStat->TotalHitNum)/((double)FtlStat->TotalLpNum);

 fprintf(outFP, "\n");
 fprintf(outFP, "FTL STATISTICS\n");
 fprintf(outFP, "------------------------------------------------------------\n\n\n");
 
 fprintf(outFP, "FTL CACHE HIT STATICSTICS\n");
 fprintf(outFP, " Total data buffer size is (#): %8u MB  \n", (FTL_BUF_NUM/2048)); 
 fprintf(outFP, " Total number of page access(#): %8u   \n", FtlStat->TotalLpNum);
 fprintf(outFP, " Total number of  hit page(#): %8u   \n", FtlStat->TotalHitNum);
 fprintf(outFP, " Total number of  hit rate(#): %f   \n\n\n", hitrate);

 fprintf(outFP, "FTL POWER CONSUMPTION STATICSTICS\n\n");
 
 FtlStat->Total_NVstatic_energy = FTLNV_STATIC_PC*(FtlStat->Total_Simtime);
 
 FtlStat->Total_NVaccess_energy = (stat_read_num+stat_gc_read_num)*FTLNV_RD_PC*FTLNV_RD_ACCTIME;
 FtlStat->Total_NVaccess_energy += ((stat_write_num+stat_gc_write_num)*(FTLNV_RD_PC*FTLNV_RD_ACCTIME+FTLNV_WR_PC*FTLNV_WR_ACCTIME));

 FtlStat->Total_Flashaccess_energy = (stat_read_num+stat_gc_read_num)*FTLFLASH_READ_PC*FTLFLASH_READ_ACCTIME;
 FtlStat->Total_Flashaccess_energy += ((stat_write_num+stat_gc_write_num)*(FTLFLASH_PROG_PC*FTLFLASH_PROG_ACCTIME));
 FtlStat->Total_Flashaccess_energy += (stat_erase_num*(FTLFLASH_ERASE_PC*FTLFLASH_ERASE_ACCTIME));
 
 FtlStat->Total_Flashstatic_energy = FTLFLASH_STATIC_PC*(FtlStat->Total_Simtime);
 
 FtlStat->Total_Dramstatic_energy = FTLDR_STATIC_PC *(FtlStat->Total_Simtime);
  
 ave_pc  = FtlStat->Total_NVaccess_energy;
 ave_pc += FtlStat->Total_NVstatic_energy;
 ave_pc += FtlStat->Total_Flashaccess_energy;
 ave_pc += FtlStat->Total_Flashstatic_energy;
 ave_pc += FtlStat->Total_Dramaccess_energy;
 ave_pc += FtlStat->Total_Dramstatic_energy;
 pc_per_page = ave_pc/FtlStat->Total_IoReq;  
 
 peak_pc = FTLNV_STATIC_PC+FTLDR_STATIC_PC+FTLFLASH_STATIC_PC;
 peak_pc += FTLNV_WR_PC;                //pcm/stt write power 
 peak_pc += FTLFLASH_PROG_PC*FTL_CE_NUM;//flash array write power
 peak_pc += FTLDR_DYN_PC;             //dram write power
 
 fprintf(outFP, " total consumed energy is : %f w\n", ave_pc);
 fprintf(outFP, " Peak power consumption is : %f w\n", peak_pc); 
 fprintf(outFP, " Average energy per io is  %f mJ\n", pc_per_page); //power of ftl controller should be included in calculation

 fprintf(outFP, " Index write num is  %d \n", index_wr_cnt); 
 fprintf(outFP, " Strip write num is  %d \n", strip_wr_cnt); 
 
 fprintf(outFP, "------------------------------------------------------------\n"); 
}

unsigned int ChkOldPBN(unsigned int lpn)
{
 unsigned int ppn;
 ppn = pagemap[lpn].ppn;
 return(ppn);
}

void UpdateEntryInPPCTable(unsigned int lpn,unsigned int ppn,int flag)
{
	int i;
	int found = 0;
	entry_t *temp;
  entry_t *temp1 = NULL;
  unsigned int parity_lpn;
 
  switch(flag){
    case RemoveEntry:
      {      	
      	if(ppc_table->UsedHead == NULL)
 	        {printf("remove parity from the empty in UpdateEntryInPPCTable!!\n"); exit(-1);}
      	if(ppc_table->UsedHead == NULL && ppc_table->UsedTail != NULL)
 	        {printf("ppc_table->UsedTail and ppc_table->UsedHead are not consistent!!\n"); exit(-1);} 	        
      	if(ppc_table->TotalNum == 0)
 	        {printf("TotalNum is NOT correct in UpdateEntryInPPCTable!!\n"); exit(-1);} 	        
        temp = ppc_table->UsedHead;
        if(lpn>=parity_blkno/SECT_NUM_PER_PAGE) parity_lpn = lpn;  
        else parity_lpn = lpn/page_per_strip + parity_blkno/SECT_NUM_PER_PAGE;
        while(temp){
          if(temp->ParityLPN == parity_lpn && temp1 == NULL) temp1 = temp;
          else if(temp->ParityLPN == parity_lpn && temp1 != NULL){printf("Multiple cache entries record the same parity lpn in UpdateEntryInPPCTable!!\n"); exit(-1);}
          temp = temp->next;   	
          }
        if(temp1 == NULL)
        	{printf("cannot find removable parity item in parity table in UpdateEntryInPPCTable!!\n"); exit(-1);}
          
        if(ppc_table->UsedHead == ppc_table->UsedTail)
        	 {ppc_table->UsedHead = NULL; ppc_table->UsedTail = NULL;}
        else if(ppc_table->UsedHead == temp1)
        	 {ppc_table->UsedHead = temp1->next;temp1->next->prev = NULL;}
        else if(ppc_table->UsedTail == temp1)
        	 {ppc_table->UsedTail = temp1->prev;temp1->prev->next = NULL;}
        else
        	{ temp1->prev->next = temp1->next; 
        	  temp1->next->prev = temp1->prev;} 
        	  
        if(ppc_table->AvailHead == NULL){ppc_table->AvailHead = temp1; ppc_table->AvailTail = temp1;temp1->next = NULL; temp1->prev = NULL;}
        else{ppc_table->AvailTail->next = temp1; temp1->prev = ppc_table->AvailTail; ppc_table->AvailTail = temp1;}
        temp1->TotalOldPBN = 0;
        temp1->AccCnt = 0;
        ppc_table->TotalNum--;break;
      }   	
    case UpdateEntry:
    	{
    		parity_lpn = lpn/page_per_strip + parity_blkno/SECT_NUM_PER_PAGE; 
      	if(ppc_table->UsedHead == NULL)
 	        {printf("updating parity for the empty buffer for UpdateEntry!!\n"); exit(-1);}
      	if(ppc_table->TotalNum == 0)
 	        {printf("Total number is NOT correct for UpdateEntry!!\n"); exit(-1);} 	        
        temp = ppc_table->UsedHead;
        for(i=0;i<ppc_table->TotalNum;i++){
          if(temp->ParityLPN == parity_lpn && temp1 == NULL) temp1 = temp;
          else if(temp->ParityLPN == parity_lpn && temp1 != NULL){printf("more than one parity item for UpdateEntry!!\n"); exit(-1);}
          temp = temp->next;   	
          }
        if(temp1 == NULL){printf("cannot find removable parity item in parity table!!\n"); exit(-1);}
         
        for(i=0;i<temp1->TotalOldPBN;i++)
          {if(temp1->ModifiedLPN[i] == lpn){found = 1; temp1->OldPBN[i] = ppn;}}        
        if(found == 0){
          temp1->ModifiedLPN[temp1->TotalOldPBN] = lpn;
          temp1->OldPBN[temp1->TotalOldPBN] = ppn;
          (temp1->TotalOldPBN)++;
        }
        temp1->acctime = simtime;
        temp1->AccCnt++;
        if(temp1->TotalOldPBN > page_per_strip){printf("temp1->TotalOldPBN is above the limit!!\n"); exit(-1);}
        break;    		
    	}
    case AddEntry:
    	{
    		parity_lpn = lpn/page_per_strip + parity_blkno/SECT_NUM_PER_PAGE; 
      	if(ppc_table->UsedHead == NULL && ppc_table->TotalNum != 0 ||
      		 ppc_table->UsedTail == NULL && ppc_table->UsedHead != NULL)
 	        {printf("ppc_table->TotalNum is NOT correct!!\n"); exit(-1);}
      	if(ppc_table->AvailHead == NULL && ppc_table->AvailTail != NULL )
 	        {printf("head pointer and tail pointer are inconsistent!!\n"); exit(-1);}
        temp = ppc_table->AvailHead;
        if(ppc_table->AvailHead == ppc_table->AvailTail)//only one entry available in bi-dir link
          {ppc_table->AvailHead = NULL;
           ppc_table->AvailTail = NULL;
          }
        else{
           ppc_table->AvailHead = ppc_table->AvailHead->next;
           ppc_table->AvailHead->prev = NULL;     
          }
        temp->next = NULL; temp->prev = NULL; temp->ParityLPN = parity_lpn;
        temp->ModifiedLPN[0] = lpn; temp->OldPBN[0] = ppn; 
        temp->TotalOldPBN = 1;
        temp->AccCnt = 1;
        temp->acctime = simtime;
        if(ppc_table->UsedHead == NULL)
          {ppc_table->UsedHead = temp;
           ppc_table->UsedTail = temp;
          }
        else{
            ppc_table->UsedTail->next = temp;
            temp->prev = ppc_table->UsedTail;
            ppc_table->UsedTail = temp;           
          }       
        ppc_table->TotalNum++;break;
      }    
  }
}

/******CommitGCPage******/

void InsertLpnToGCqueue(unsigned int victim_blk_no, int valid_flag,unsigned int tar_lpn, unsigned int pbn,int page_no)
{
 entry_t *temp_entry;
 fltiob *temp; 
 unsigned int tar_lpn = -1; 
 
 tar_lpn = victim_blk_no/SECT_NUM_PER_PAGE;
 
 switch(cache_policy){
 case PPC: 
 case CHUNKPPC:
 	         temp = FtlChkBufHit(victim_blk_no); 
  	       if(temp != NULL)
  	       	{
             temp_entry = (entry_t *)malloc(sizeof(entry_t));
             temp_entry->ParityLPN = tar_lpn;
             temp_entry->ModifiedLPN[0] = valid_flag;
             if(gc_page_table->UsedHead == NULL && gc_page_table->UsedHead != NULL ||
             	  gc_page_table->UsedHead == NULL && gc_page_table->TotalNum != 0)
             	  {printf("gc_page_table->UsedHead and gc_page_table->UsedTail are not consistent in InsertLpnToGCqueue!!\n"); exit(-1);}
             
             if(gc_page_table->UsedHead == NULL){
             	  gc_page_table->UsedHead = temp_entry;gc_page_table->UsedTail = temp_entry;
             	  temp_entry->next = NULL; temp_entry->prev = NULL;}
             else{
             	  gc_page_table->UsedTail->next = temp_entry;
             	  temp_entry->next = NULL; temp_entry->prev = gc_page_table->UsedTail;
             	  gc_page_table->UsedTail = temp_entry;} 		  
             	
                gc_page_table->TotalNum++;
            }
           break;
 default: break;}   	
}

unsigned int CheckGCPageQueue()
{
 unsigned int GClpn = 0;
 entry_t *temp_entry;
 
 temp_entry = gc_page_table->UsedHead; 
 
 if(gc_page_table->UsedHead == NULL && gc_page_table->UsedTail != NULL ||
 	  gc_page_table->UsedHead == NULL && gc_page_table->TotalNum != 0)
 	  {printf("gc_page_table->UsedHead and gc_page_table->UsedTail are not consistent in InsertLpnToGCqueue!!\n"); exit(-1);}
 	  
 if(temp_entry == NULL){return(0);}
 else{
  GClpn = ((temp_entry->ParityLPN)<< 1) | (temp_entry->ModifiedLPN[0] & 0x1);
  if(gc_page_table->UsedHead == gc_page_table->UsedTail) {
     gc_page_table->UsedHead = NULL;
     gc_page_table->UsedTail = NULL;}
  else if(gc_page_table->UsedHead == temp_entry)
  	 {gc_page_table->UsedHead = temp_entry->next;gc_page_table->UsedHead->prev = NULL;}
  else if(gc_page_table->UsedTail == temp_entry)
     {gc_page_table->UsedTail = temp_entry->prev;gc_page_table->UsedTail->next = NULL;}
  else{
       gc_page_table->UsedHead = temp_entry->next;
       temp_entry->next->prev = NULL;}}
 
 free(temp_entry);
 gc_page_table->TotalNum--;
 return(GClpn);
}
       	 
double CommitGCReadPage()
{
 unsigned int GClpn = 0;
 int valid_flag = 0;
 
 double overhead_temp;
 double overhead = 0;
 
 GClpn = CheckGCPageQueue();
  
 while(GClpn)
 {
  valid_flag = GClpn & 0x1;
  GClpn = GClpn>>1; 

  overhead_temp = FtlGetIobOverhead(valid_flag,GClpn);
  overhead = max(overhead,overhead_temp);
  GClpn = CheckGCPageQueue();
 }
 return(overhead);
}

int ChkPBATable(unsigned int lpn)
{
	int i,j;
	int find = 0;
	entry_t *temp;
  entry_t *temp1 = NULL;
  unsigned int parity_lpn;
 
  if(ppc_table->UsedHead == NULL && ppc_table->TotalNum != 0)
  	{printf("UsedHead is not correct in ChkPBATable!!\n"); exit(-1);}
 	if(ppc_table->TotalNum != 0){
  temp = ppc_table->UsedHead;
  for(i=0;i<ppc_table->TotalNum;i++){
  	for(j=0;j<temp->TotalOldPBN;j++){
    if(temp->ModifiedLPN[j] == lpn && temp1 == NULL) {
    	 temp1 = temp; 
    	 if(j == 0) find = 1;
    	 else find = 2;}
    else if(temp->ModifiedLPN[j] == lpn && temp1 != NULL)
    	{printf("Multiple cache entries record the same parity lpn in ChkPBATable!!\n"); exit(-1);}  	
    }      
    temp = temp->next; }
  }
  
  if(temp1 != NULL && temp1== ppc_table->UsedTail && find == 1) find = find + 2;
  return(find);
}

int UpdateEntryInPBATable(unsigned int lpn,unsigned int next_ppn,unsigned int prev_ppn, int flag)
{
	int i;
	int found = 0;
	entry_t *temp;
  entry_t *temp1 = NULL;
  unsigned int parity_lpn;
 
  switch(flag){
    case RemoveEntry:
      {      	
      	if(ppc_table->UsedHead == NULL)
 	        {printf("remove parity from the empty in UpdateEntryInPBATable!!\n"); exit(-1);}
      	if(ppc_table->UsedHead == NULL && ppc_table->UsedTail != NULL)
 	        {printf("ppc_table->UsedTail and ppc_table->UsedHead are not consistent!!\n"); exit(-1);} 	        
      	if(ppc_table->TotalNum == 0)
 	        {printf("TotalNum is NOT correct in UpdateEntryInPBATable!!\n"); exit(-1);} 	        
        temp = ppc_table->UsedHead;
        while(temp){
          if(temp->ModifiedLPN[0] == lpn && temp1 == NULL) temp1 = temp;
          else if(temp->ModifiedLPN[0] == lpn && temp1 != NULL){printf("Multiple cache entries record the same parity lpn in UpdateEntryInPBATable!!\n"); exit(-1);}
          temp = temp->next;   	
          }
        if(temp1 == NULL)
        	{printf("cannot find removable parity item in parity table in UpdateEntryInPBATable!!\n"); exit(-1);}
          
        if(ppc_table->UsedHead == ppc_table->UsedTail)
        	 {ppc_table->UsedHead = NULL; ppc_table->UsedTail = NULL;}
        else if(ppc_table->UsedHead == temp1)
        	 {ppc_table->UsedHead = temp1->next;temp1->next->prev = NULL;}
        else if(ppc_table->UsedTail == temp1)
        	 {ppc_table->UsedTail = temp1->prev;temp1->prev->next = NULL;}
        else
        	{ temp1->prev->next = temp1->next; 
        	  temp1->next->prev = temp1->prev;} 
        	  
        if(ppc_table->AvailHead == NULL){ppc_table->AvailHead = temp1; ppc_table->AvailTail = temp1;temp1->next = NULL; temp1->prev = NULL;}
        else{ppc_table->AvailTail->next = temp1; temp1->prev = ppc_table->AvailTail; ppc_table->AvailTail = temp1;}
        temp1->TotalOldPBN = 0;
        ppc_table->TotalNum--;
        //printf("remove check!!\n");      
        //ChkPBATable(lpn);
        break;
      }   	
    case UpdateEntry:
    	{
      	if(ppc_table->UsedHead == NULL)
 	        {printf("updating parity for the empty buffer for UpdateEntry for UpdateEntryInPBATable!!\n"); exit(-1);}
      	if(ppc_table->TotalNum == 0)
 	        {printf("Total number is NOT correct for UpdateEntry for UpdateEntryInPBATable!!\n"); exit(-1);} 	        
      	if(ppc_table->UsedTail->TotalOldPBN > page_per_strip+1)
 	        {printf("TotalOldPBN is NOT correct for UpdateEntry for UpdateEntryInPBATable!!\n"); exit(-1);} 	        
        
        temp = ppc_table->UsedTail;
        temp->ModifiedLPN[temp->TotalOldPBN] = lpn;
        temp->TotalOldPBN++;        
        temp->acctime = simtime;
        //printf("UpdateEntry check!!\n");              
        //ChkPBATable(lpn);        
        break;    		
    	}
    case AddEntry:
    	{
      	if(ppc_table->UsedHead == NULL && ppc_table->TotalNum != 0 ||
      		 ppc_table->UsedTail == NULL && ppc_table->UsedHead != NULL)
 	        {printf("ppc_table->TotalNum is NOT correct!!\n"); exit(-1);}
      	if(ppc_table->AvailHead == NULL && ppc_table->AvailTail != NULL)
 	        {printf("head pointer and tail pointer are inconsistent for UpdateEntryInPBATable!!\n"); exit(-1);}
 	      if(ppc_table->AvailHead != NULL){
          temp = ppc_table->AvailHead;
          found = 1;
          if(ppc_table->AvailHead == ppc_table->AvailTail)//only one entry available in bi-dir link
            {ppc_table->AvailHead = NULL;
             ppc_table->AvailTail = NULL;
            }
          else{
             ppc_table->AvailHead = ppc_table->AvailHead->next;
             ppc_table->AvailHead->prev = NULL;}
          temp->next = NULL; temp->prev = NULL;
          temp->ModifiedLPN[0] = lpn; 
          temp->TotalOldPBN = 1;
          temp->acctime = simtime;
          if(ppc_table->UsedHead == NULL)
          {ppc_table->UsedHead = temp;
           ppc_table->UsedTail = temp;
          }
          else{
             ppc_table->UsedTail->next = temp;
             temp->prev = ppc_table->UsedTail;
             ppc_table->UsedTail = temp;}       
          ppc_table->TotalNum++;
          //printf("AddEntry check!!\n");  
          //ChkPBATable(lpn);                                
          break;
         }
      }    
  }
  return(found);
}

int ChkRaidBlk(unsigned int ppn)
{
 int flag = 0;
 int bit_flag;
 
 bit_flag = (ppn>>(FTL_CHNL_WIDTH+FTL_CHNL_WIDTH+PAGE_BITS))& 0x1;

 return(bit_flag); 
}

int ChkStripeNum(unsigned int lbn, unsigned int num)
{
 int i;
 int hit_num =0;
 int chk_num;
 unsigned int start_lbn;
 start_lbn = (lbn/page_per_strip)*page_per_strip;

 for(i=0;i<page_per_strip;i++){
   if(start_lbn >= lbn && start_lbn < (lbn+num))hit_num++;
   start_lbn++;
 }
   
 return(hit_num);
}

int ChkHitMemNum(unsigned int lpn, int num)
{
	int page_num = 0;
	int i,j;
	entry_t *temp;
	entry_t *temp1;
	unsigned int parity_lpn;
	
	temp = ppc_table->UsedHead;
	temp1 = NULL;
	parity_lpn = (lpn/page_per_strip)+ parity_blkno/SECT_NUM_PER_PAGE;
	
	for(i=0;i<ppc_table->TotalNum;i++)
	 {
	 	if(temp == NULL){printf("TotalNum is not consistent with usedlink!!\n"); exit(-1);}
	 	if(temp->ParityLPN == parity_lpn && temp1 == NULL) temp1 = temp; 
	  else if(temp->ParityLPN == parity_lpn && temp1 != NULL)
	  	{printf("parity has been hit in ChkHitMemNum!!\n"); exit(-1);}
	 	temp = temp->next;}
	if(temp1 == NULL){printf("cannot find matched lpn in ChkHitMemNum!!\n"); exit(-1);}
		
	for(i=0;i<num;i++){
  	for(j=0;j<temp1->TotalOldPBN;j++){
  		if(temp1->ModifiedLPN[j] == (lpn+i))page_num++;
    }}

	return(page_num);  
}

double CommitCurrParity(unsigned int parity_lpn, fltiob *curriob){

double overhead=0;
double ftlaccoverhead;
double raid_read_overhead=0;


    raid_read_overhead = GetRaidReadOverhead(parity_lpn,NORMAL_LRU);
    overhead = FtlCplReq(parity_lpn*SECT_NUM_PER_PAGE,(curriob->flags & READ),curriob); 
    curriob->availtime = overhead + simtime;       
    curriob->state = FTL_BUF_CPL;  
    
    FtlRemovedLRUCplIob(curriob);
    UpdateEntryInPPCTable(parity_lpn,0,RemoveEntry);
    FtlAddNewUsedIob(parity_lpn*SECT_NUM_PER_PAGE,WRITE);
    ftlaccoverhead = max(raid_read_overhead,overhead);
 
 return(ftlaccoverhead);	
}


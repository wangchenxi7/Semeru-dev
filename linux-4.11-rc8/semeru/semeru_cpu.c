/**
 * The main entry of Semeru CPU server module
 *  
 */


#include "semeru_cpu.h"




MODULE_AUTHOR("Semeru,Chenxi Wang");
MODULE_DESCRIPTION("RMEM, remote memory paging over RDMA");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION("1.0");




// invoked by insmod 
int __init semeru_cpu_init(void){

  int ret = 0;

  #ifdef SEMERU_FRONTSWAP_PATH

    ret = semeru_fs_rdma_client_init();
    if(unlikely(ret)){
      printk(KERN_ERR "%s, semeru_fs_rdma_client_init failed. \n",__func__);
      goto out;
    }

  #else
    printk(KERN_ERR "%s, TO BE DONE.\n",__func__);

  #endif


out:
	return ret;
}


// invoked by rmmod 
void __exit semeru_cpu_exit(void)
{
  	
	int ret;
	
	printk(" Prepare to remove the Semeru CPU Server module.\n");

  #ifdef SEMERU_FRONTSWAP_PATH
    semeru_fs_rdma_client_exit();
  #else
    printk(KERN_ERR "%s, TO BE DONE.\n",__func__);
  #endif
	

	printk(" Remove  Semeru CPU Server module DONE. \n");

	return;
}

module_init(semeru_cpu_init);
module_exit(semeru_cpu_exit);


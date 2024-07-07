#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <asm/unistd.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/perf_event.h>
#include "perf.c"
#define N 1024
static int matrixA[N][N];
static int matrixB[N][N];
static int matrixC[N][N];
//initialise vectors with random values
void init_matrixes(){
	int i, j;
	unsigned int p = 0;
	for (i = 0; i < N; i++)
		for (j = 0; j < N; j++){
			p+= 65610001;		  	//increment by prime number
			matrixA[i][j] = p%1336337;	//mod prime number
			matrixB[i][j] = p%4477457;	//mod prime number
			matrixC[i][j] = 0;
		}
}

void matrix_multiply_basic()
{
	int i,j,k;
	for (i = 0 ; i < N ; i+=1)
		for(j = 0 ; j < N ; j +=1)
			for(k = 0 ; k < N ; k +=1)
				matrixC[i][j] += matrixA[i][k] * matrixB[k][j];
}

void matrix_multiply_asm()
{
    printf("In matmul asm function ... \n");
    /*
     * Using registers (r0-r12, r14)
     * Control variables:
     *  - r0 = i, r1 = j, r2 = k
     * Base Address: 
     *  - r3 = &matrixA[i][j],
     *  - r4 = &matrixB[i][j],
     *  - r5 = &matrixC[i][j],
     * Data:
     *  - r6 = matrixA data 
     *  - r7 = matrixB data 
     * Pointer to the start of A's target row:
     *  - r8 = (1024 * 4 * i) + r3
     *  - When moving to next, +4 bytes
     * Pointer to the start of B's target column:
     *  - r10 = (4 * j) + r4
     *  - when moving next +4096 bytes
     * Constant 1024*4 (offset is in bytes!): try immediate here???
     *  - r12 = 4096 
     *  - r14 = 4
     * Temp_accummulator: r9
     * Free: r11
     * */
    asm volatile(
		"push {r0-r12, r14}"			"\n\t"	//save the state of the registers
                                    // initialise base address registers and frequently used offset constant 
        "mov r3, %[matrixA_addr]"       "\n\t" // matrixA base address
        "mov r4, %[matrixB_addr]"       "\n\t" // matrixB base address
        "mov r5, %[matrixC_addr]"       "\n\t" // matrixC base address
        "mov r12, #4096"                "\n\t" // frequently used constant, 1024*4, used to get A's target ROW 
        "mov r14, #4"                   "\n\t" // frequently used constant, 4, used to get B's target COLUMN

        "mov r0, #0"                    "\n\t" // i = 0
    "I_LOOP:"                           "\n\t"
        "cmp r0, #1024"                 "\n\t" // Have we reached i=1024?
        "beq END"                       "\n\t" //  - if YES, go to end
                                               //  - if NO, continue
        "mov r1, #0"                    "\n\t" // j = 0
    "J_LOOP:"                           "\n\t"
        "cmp r1, #1024"                 "\n\t" // Have we reached j=1024?
        "beq NEXT_I_ITERATION"          "\n\t" //  - if YES, go to next I-ITERATION
                                               //  - if NO, continue
        "mla r8, r12, r0, r3"           "\n\t" // get the address pointing to the start of A's target ROW 
        "mov r2, #0"                    "\n\t" // k = 0
        "mla r10, r14, r1, r4"          "\n\t" // get the address pointing to the start of B's target COLUMN 
        "mov r9, #0"                    "\n\t" // initialise temp_accummulator, r9 = 0
    "K_LOOP:"                           "\n\t"
        "ldr r6, [r8]"                  "\n\t" // load A's target element 
        "ldr r7, [r10]"                 "\n\t" // load B's target element
        "mla r9, r6, r7, r9"            "\n\t" // r9 = (r6 * r7) + r9
                                    // move A and B to next target elements 
        "add r8, r8, r14"               "\n\t" // next A element address, +4 bytes
        "add r10, r10, r12"             "\n\t" // next B element address, +4096 bytes
                                    // k + 1 
        "add r2, r2, #1"                "\n\t" // k++ 
        "cmp r2, #1024"                 "\n\t" // Have we reached k=1024 yet? 
        "bne K_LOOP"                    "\n\t" //  - if NOT, go to next K iteration
        "add r1, r1, #1"                "\n\t" //  - if YES, store C[i][j] <- *r9, and go to next J iteration, j++
        "str r9, [r5], #4"              "\n\t" //    NOTE r5, C[i][j] is auto incrementing here   
        "b J_LOOP"                      "\n\t"  

    "NEXT_I_ITERATION:"                 "\n\t"
        "add r0, r0, #1"                "\n\t" // i++
        "b I_LOOP"                      "\n\t" // go to next I loop
    "END:"                              "\n\t"
		"pop {r0-r12, r14}"			    "\n\t" //retrieve the state of the registers

		:					//list of outputs (empty, because the output is directly in memory
		: [matrixA_addr] "r" (&matrixA[0][0]), [matrixB_addr] "r" (&matrixB[0][0]),	[matrixC_addr] "r" (&matrixC[0][0]) //list of inputs
		: "r3", "r4", "r5") ; //list of changed registers (Base address registers will not be used by compiler)
}

int main()
{
    printf("Running matmul ... \n");
	init_matrixes();/* Initializes matrix elements */
	int perf;
	
	// init performance events count
	//for description of events write "man perf_event_open" in console
	perf = setup_perf(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES); 
	//perf = setup_perf_cache(PERF_COUNT_HW_CACHE_LL, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_MISS);
	
	clock_t begin_time = clock();
	
	start_perf(perf);		// start performance events count
	
    //matrix_multiply_basic();
    matrix_multiply_asm();
	
	end_perf(perf);			// stop performance events count
	
	clock_t end_time = clock();
	
	printf("Execution took %f seconds.\n", (end_time-begin_time)/1000000.0);
	
	print_perf(perf);		// print performance events count

	return 0;
}



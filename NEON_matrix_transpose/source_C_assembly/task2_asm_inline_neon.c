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
static int matrixA[N][N] __attribute__ ((aligned(64))); //64-bit alignment of data on RAM
static int matrixB[N][N] __attribute__ ((aligned(64))); //64-bit alignment of data on RAM
static int matrixC[N][N];
//initialise vectors with random values
void init_matrixes(){
	int i, j, temp;
	unsigned int p = 0;
	for (i = 0; i < N; i++) {
		for (j = 0; j < N; j++){
			p+= 65610001;		  	//increment by prime number
			matrixA[i][j] = p%1336337;	//mod prime number
			matrixB[i][j] = p%4477457;	//mod prime number
			matrixC[i][j] = 0;
		}
    }

    // transpose B, make it easier for vector vld instruction
    // if all the request data stays in a row, i.e. in the same cache line
    for (i = 0; i < N; i++) {
        for (j = i; j < N; j++) {
            temp = matrixB[i][j];
            matrixB[i][j] = matrixB[j][i];
            matrixB[j][i] = temp;
        }
    }
}

#if 0
void matrix_multiply_basic()
{
	int i,j,k;
	for (i = 0 ; i < N ; i+=1)
		for(j = 0 ; j < N ; j +=1)
			for(k = 0 ; k < N ; k +=1)
				matrixC[i][j] += matrixA[i][k] * matrixB[k][j];
}
#endif

void matrix_multiply_asm_neon()
{
    printf("In matmul asm function ... \n");
    /*
     * Using registers (r0-r12, r14)
     * Control variables:
     *  - r0 = i, r1 = j, r2 = k
     * Base Address: 
     *  - r3 = &A00, r4 = &B00, r5= = &C00
     * Pointer to the start of A's target row:
     *  - r8 = (1024 * 4 * i) + r3, When moving to next, +4 bytes
     * Pointer to the start of B's target row:
     *  - r10 = (1024 * 4 * j) + r4, When moving next +4096 bytes
     * Constant 1024*4 (offset is in bytes!): try immediate here???
     *  - r12 = 4096 
     * Pointer to element of A and B respectively:
     *  - r6: pointing to A target element; r7: pointing to B target element
     * Free: r9, r11, r14
     * q0-q7: temp_product, q8-q11: temp_sum of q0-q7 (reduce to q8, which in turn will be written to target &C)
     * we only apply vector arithematic for inner loop k using chunk of 16 integers per iteration.
     * Hence, the new limit for k is: 1024/6 = 64
     * */
    asm volatile(
		"push {r0-r12, r14}"			"\n\t"	//save the state of the registers
                                    // initialise base address registers and frequently used offset constant 
        "mov r3, %[matrixA_addr]"       "\n\t" // matrixA base address
        "mov r4, %[matrixB_addr]"       "\n\t" // matrixB base address
        "mov r5, %[matrixC_addr]"       "\n\t" // matrixC base address
        "mov r12, #4096"                "\n\t" // frequently used constant, 1024*4, used to get A/B target ROW 

        "mov r0, #0"                    "\n\t" // i = 0
    "I_LOOP:"                           "\n\t"
        "cmp r0, #1024"                 "\n\t" // Have we reached i=1024?
        "beq END"                       "\n\t" //  - if YES, go to end
                                               //  - if NO, continue
        "mla r8, r12, r0, r3"           "\n\t" // moving to NEXT A's target ROW 
        "mov r1, #0"                    "\n\t" // j = 0
    "J_LOOP:"                           "\n\t"
        "cmp r1, #1024"                 "\n\t" // Have we reached j=1024?
        "beq NEXT_I_ITERATION"          "\n\t" //  - if YES, go to next I-ITERATION
                                               //  - if NO, continue
        "mov r2, #0"                    "\n\t" // k = 0
        "mla r10, r12, r1, r4"          "\n\t" // moving to NEXT B's target ROW 
        "mov r6, r8"                    "\n\t" // reset the pointer to the beginning of A's target ROW
        "mov r7, r10"                   "\n\t" // reset the pointer to the beginning of B's target ROW 
    "K_LOOP:"                           "\n\t"
        "vld1.64 {d0-d3}, [r6:64]!"    "\n\t" // load first 8 elements from A's target row and increment addr 
        "vld1.64 {d4-d7}, [r7:64]!"    "\n\t" // load first 8 elements from B's target row and increment addr
        "vld1.64 {d8-d11}, [r6:64]!"   "\n\t" // load second 8 elements from A's target row and increment addr 
        "vld1.64 {d12-d15}, [r7:64]!"  "\n\t" // load second 8 elements from B's target row and increment addr
                                    // do the vector multiplication and store the result into temp_product register
        "vmul.i32 q8, q0, q4"          "\n\t" // q8 <- q0 * q4
        "vmul.i32 q9, q1, q5"          "\n\t" // q9 <- q1 * q5
        "vmul.i32 q10, q2, q6"         "\n\t" // q10 <- q2 * q6
        "vmul.i32 q11, q3, q7"         "\n\t" // q11 <- q3 * q7
                                    // reduce everything to d16[0] in q8
        "vadd.i32 q10, q10, q11"        "\n\t" // q10 <- q10 + q11
        "vadd.i32 q8, q8, q9"           "\n\t" // q8 <- q8 + q9
        "vadd.i32 q8, q8, q10"          "\n\t" // q8 <- q8 + q10
        "vadd.i32 d16, d16, d17"        "\n\t" // reduce within q8
        "vpadd.i32 d16, d16"            "\n\t" // reduce within d16 in q8, reduce d16 to least signft of d16
        "add r2, r2, #1"                "\n\t" // k++
        "cmp r2, #64"                   "\n\t" // Have we reached k=64 yet?
        "bne K_LOOP"                    "\n\t" //   - if NOT, go to next K iteration
        "vst1.32 d16[0], [r5:32]!"      "\n\t" //   - if YES, store C[i][j] <- d16[0], and go to next J iteration.
                                               //     r5, address pointer to C is auto incrementing
        "add r1, r1, #1"                "\n\t" // j++
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
    matrix_multiply_asm_neon();
	
	end_perf(perf);			// stop performance events count
	
	clock_t end_time = clock();
	
	printf("Execution took %f seconds.\n", (end_time-begin_time)/1000000.0);
	
	print_perf(perf);		// print performance events count

	return 0;
}



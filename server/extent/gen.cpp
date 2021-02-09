#include<stdio.h>
#include<string.h>
#include<stdlib.h>
int main(int argc, char* argv[]){

	if(atoi(argv[1])==1){
		for(int i=0;i<1000;i++){
			printf("11 %d\n",i);
		}
		for(int i=0;i<1200;i++){
			printf("12 %d\n",i);
		}
	}else{
		printf("1 0 1000/n");
		for(int i=0;i<1200;i++){
			printf("2 %d\n",i);
		}
	}
	printf("-1");
}

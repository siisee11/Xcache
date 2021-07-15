#include <cstdlib>
#include <stdio.h>
#include <cstdint>
#include <unistd.h>
#include <fstream>
#include <ctime>
#include <algorithm>
#include <vector>

using namespace std;

inline int64_t myrandom(int64_t i){
    return rand() % i;
}

int main(){
    srand(unsigned(time(NULL)));
    int num = 1000000000;

    vector<int64_t> keys;
    for(int64_t i=1; i<=num; i++){
	keys.push_back(i);
    }

    ofstream ofs;
    ofs.open("input_1to1.txt");
    for(int64_t i=1; i<=num; i=i+1){
	ofs << 1 << '\n';
	ofs << i << '\n';
    }
    ofs.close();

    ofs.open("input_1to2.txt");
    for(int64_t i=1; i<=num; i=i+2){
	ofs << 1 << '\n';
	ofs << i << '\n';
	ofs << i + 1 << '\n';
    }
    ofs.close();

    ofs.open("input_1to4.txt");
    for(int64_t i=1; i<=num; i=i+4){
	ofs << 1 << '\n';
	ofs << i << '\n';
	ofs << i + 1 << '\n';
	ofs << i + 2 << '\n';
	ofs << i + 3 << '\n';
    }
    ofs.close();

    ofs.open("input_1to8.txt");
    for(int64_t i=1; i<=num; i=i+8){
	ofs << 1 << '\n';
	ofs << i << '\n';
	ofs << i + 1 << '\n';
	ofs << i + 2 << '\n';
	ofs << i + 3 << '\n';
	ofs << i + 4 << '\n';
	ofs << i + 5 << '\n';
	ofs << i + 6 << '\n';
	ofs << i + 7 << '\n';
    }
    ofs.close();

    /*
    ofstream ofs;
    ofs.open("input_skew.txt");
    for(int64_t i=1; i<=num/2; i++){
	ofs << 1 << '\n';
	ofs << i << '\n';
    }
    ofs.close();
    

    ofs.open("input_sort.txt");
    for(int64_t i=1; i<=num; i++){
	ofs << i << '\n';
    }
    ofs.close();
    
    random_shuffle(keys.begin(), keys.end(), myrandom);
    ofs.open("input_rand.txt");
    for(vector<int64_t>::iterator iter=keys.begin(); iter!=keys.end(); ++iter){
	ofs << *iter << '\n';
    }
    ofs.close();
    */

    return 0;
}



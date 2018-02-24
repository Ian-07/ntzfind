/* ntzfind 2.0 (horizontal shift not included in this version)
** A spaceship search program by "zdr" with modifications by Matthias Merzenich and Aidan Pierce
**
** Warning: this program uses a lot of memory (especially for wide searches).
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "nttable.c"

#define BANNER "ntzfind 2.0 by \"zdr\", Matthias Merzenich, and Aidan Pierce, 29 November 2017"
#define FILEVERSION ((unsigned long) 2016122101)  //yyyymmddnn, same as last qfind release (differs from the above)

#define MAXPERIOD 30
#define MAXWIDTH 10  // increasing this requires a few other changes
#define MIN_DUMP 20
#define DEFAULT_DEPTH_LIMIT 2000
#define NUM_PARAMS 13

#define P_RULE 0
#define P_WIDTH 1
#define P_PERIOD 2
#define P_OFFSET 3
#define P_DEPTH_LIMIT 4
#define P_SYMMETRY 5
#define P_MAX_LENGTH 6
#define P_INIT_ROWS 7
#define P_FULL_PERIOD 8
#define P_NUM_SHIPS 9
#define P_FULL_WIDTH 10
#define P_REORDER 11
#define P_DUMP 12

#define SYM_ASYM 1
#define SYM_ODD 2
#define SYM_EVEN 3
#define SYM_GUTTER 4

/* get_cpu_time() definition taken from
** http://stackoverflow.com/questions/17432502/how-can-i-measure-cpu-time-and-wall-clock-time-on-both-linux-windows/17440673#17440673
*/

//  Windows
#ifdef _WIN32
#include <Windows.h>
double get_cpu_time(){
    FILETIME a,b,c,d;
    if (GetProcessTimes(GetCurrentProcess(),&a,&b,&c,&d) != 0){
        //  Returns total user time.
        //  Can be tweaked to include kernel times as well.
        return
            (double)(d.dwLowDateTime |
            ((unsigned long long)d.dwHighDateTime << 32)) * 0.0000001;
    }else{
        //  Handle error
        return 0;
    }
}

//  Posix/Linux
#else
#include <time.h>
double get_cpu_time(){
    return (double)clock() / CLOCKS_PER_SEC;
}
#endif

int sp[NUM_PARAMS];
uint32_t *gInd, *pInd;
uint32_t *pRemain;
uint32_t *gcount;
uint16_t *gRows, *pRows;
uint16_t *ev2Rows;               // lookup table that gives the evolution of a row with a blank row above and a specified row below
int *lastNonempty;
unsigned long long dumpPeriod;
int bc[8] = {0, 1, 1, 2, 1, 2, 2, 3};
char *buf;

int rule, period, offset, width, rowNum, loadDumpFlag;
int shipNum, firstFull;
uint16_t fpBitmask = 0;

int phase, fwdOff[MAXPERIOD], backOff[MAXPERIOD], doubleOff[MAXPERIOD], tripleOff[MAXPERIOD];

void makePhases(){
   int i;
   for (i = 0; i < period; i++) backOff[i] = -1;
   i = 0;
   for (;;) {
      int j = offset;
      while (backOff[(i+j)%period] >= 0 && j < period) j++;
      if (j == period) {
         backOff[i] = period-i;
         break;
      }
      backOff[i] = j;
      i = (i+j)%period;
   }
   for (i = 0; i < period; i++)
      fwdOff[(i+backOff[i])%period] = backOff[i];
   for (i = 0; i < period; i++) {
      int j = (i - fwdOff[i]);
      if (j < 0) j += period;
      doubleOff[i] = fwdOff[i] + fwdOff[j];
   }
   for (i = 0; i <  period; i++){
      int j = (i - fwdOff[i]);
      if (j < 0) j += period;
      tripleOff[i] = fwdOff[i] + doubleOff[j];
   }
}

/*
** For each possible phase of the ship, equivRow[phase] gives the row that 
** is equivalent if the pattern is subperiodic with a specified period.
** equivRow2 is necessary if period == 12, 24, or 30, as then two subperiods
** need to be tested (e.g., if period == 12, we must test subperiods 4 and 6).
** twoSubPeriods is a flag that tells the program to test two subperiods.
*/

int equivRow[MAXPERIOD];
int equivRow2[MAXPERIOD];
int twoSubPeriods = 0;

int gcd(int a, int b){
   int c;
   while (b){
      c = b;
      b = a % b;
      a = c;
   }
   return a;
}

int smallestDivisor(int b){
   int c = 2;
   while(b % c) ++c;
   return c;
}

void makeEqRows(int maxFactor, int a){
   int tempEquivRow[MAXPERIOD];
   int i,j;
   for(i = 0; i < period; ++i){
      tempEquivRow[i] = i;
      for(j = 0; j < maxFactor; ++j){
         tempEquivRow[i] += backOff[tempEquivRow[i] % period];
      }
      tempEquivRow[i] -= offset * maxFactor + i;
      if(a == 1) equivRow[i] = tempEquivRow[i];
      else equivRow2[i] = tempEquivRow[i];
   }
   for(i = 0; i < period; ++i){     // make equivRow[i] negative if possible
      if(tempEquivRow[i] > 0){
         if(a == 1) equivRow[i + tempEquivRow[i]] = -1 * tempEquivRow[i];
         else equivRow2[i + tempEquivRow[i]] = -1 * tempEquivRow[i];
      }
   }
}

int evolveBit(int row1, int row2, int row3, int bshift){
   return nttable[(((row2>>bshift) & 2)<<7) | (((row1>>bshift) & 2)<<6)
                | (((row1>>bshift) & 4)<<4) | (((row2>>bshift) & 4)<<3)
                | (((row3>>bshift) & 7)<<2) | (((row2>>bshift) & 1)<<1)
                |  ((row1>>bshift) & 1)<<0];
}

int evolveRow(int row1, int row2, int row3){
   int row4;
   int row1_s,row2_s,row3_s;
   int j,s = 0;
   if(sp[P_SYMMETRY] == SYM_ODD) s = 1;
   if(evolveBit(row1, row2, row3, width - 1)) return -1;
   if(sp[P_SYMMETRY] == SYM_ASYM && evolveBit(row1 << 2, row2 << 2, row3 << 2, 0)) return -1;
   if(sp[P_SYMMETRY] == SYM_ODD || sp[P_SYMMETRY] == SYM_EVEN){
      row1_s = (row1 << 1) + ((row1 >> s) & 1);
      row2_s = (row2 << 1) + ((row2 >> s) & 1);
      row3_s = (row3 << 1) + ((row3 >> s) & 1);
   }
   else{
      row1_s = (row1 << 1);
      row2_s = (row2 << 1);
      row3_s = (row3 << 1);
   }
   row4 = evolveBit(row1_s, row2_s, row3_s, 0);
   for(j = 1; j < width; j++)row4 += evolveBit(row1, row2, row3, j - 1) << j;
   return row4;
}

void sortRows(uint32_t rowSet){
   uint32_t totalRows = gInd[rowSet + 1] - gInd[rowSet];
   uint16_t *row = &(gRows[gInd[rowSet]]);
   uint32_t i;
   int64_t j;
   uint16_t t;
   for(i = 1; i < totalRows; ++i){
      t = row[i];
      j = i - 1;
      while(j >= 0 && gcount[row[j]] < gcount[t]){
         row[j+1] = row[j];
         --j;
      }
      row[j+1] = t;
   }
}

void makeTables(){
   printf("\nBuilding lookup tables... ");
   gInd = malloc(((long long)4 << (width * 3)) + 4);
   ev2Rows = malloc((long long)sizeof(*ev2Rows) * (1 << (width * 2)));
   gcount = malloc((long long)sizeof(*gcount) * (1 << width));
   uint32_t i;
   int row1,row2,row3,row4;
   long int rows123,rows124;
   uint32_t numValid = 0;
   for(i = 0; i < 1 << width; ++i) gcount[i] = 0;
   for(i = 0; i < ((1 << (3 * width)) + 1); i++)gInd[i] = 0;
   rows123 = -1;     //represents row1, row2, and row3 stacked vertically
   for(row1 = 0; row1 < 1 << width; row1++)for(row2 = 0; row2 < 1 << width; row2++)for(row3 = 0; row3 < 1 << width; row3++){
      rows123++;
      row4 = evolveRow(row1,row2,row3);
      if(row4 < 0) continue;
      ++gcount[row4];
      if(row1 == 0) ev2Rows[rows123] = row4;
      gInd[rows123 - row3 + row4]++;
      numValid++;
   }
   gRows = malloc(2 * numValid);
   for(rows124 = 1; rows124 < 1 << (3 * width); rows124++) gInd[rows124] += gInd[rows124 - 1];
   gInd[1 << (3 * width)] = gInd[(1 << (3 * width)) - 1];  //extra needed for last set to calculate number
   rows123 = -1;
   for(row1 = 0; row1 < 1 << width; row1++)for(row2 = 0; row2 < 1 << width; row2++)for(row3 = 0; row3 < 1 << width; row3++){
      rows123++;
      row4 = evolveRow(row1,row2,row3);
      if(row4 < 0) continue;
      rows124 = rows123 - row3 + row4;
      gInd[rows124]--;
      gRows[gInd[rows124]] = (uint16_t)row3;
   }
   printf("Lookup tables built.\n");
   
   gcount[0] = 0;
   if(sp[P_REORDER]){
      printf("Sorting lookup table..... ");
      for(rows124 = 0; rows124 < 1 << (3 * width); ++rows124){
         sortRows(rows124);
      }
      printf("Lookup table sorted.\n");
   }
   free(gcount);
}

void printInfo(int currentDepth, unsigned long long numCalcs, double runTime){
   if(currentDepth >= 0) printf("Current depth: %d\n", currentDepth - 2*period);
   printf("Calculations: ");
   if(numCalcs > 1000000000)printf("%lluM\n", numCalcs / 1000000);
   else printf("%llu\n", numCalcs);
   printf("CPU time: %f seconds\n",runTime);
   fflush(stdout);
}

void buffPattern(int theRow){
   int firstRow = 2 * period;
   if(sp[P_INIT_ROWS]) firstRow = 0;
   int lastRow;
   int i, j;
   char *out = buf;
   for(lastRow = theRow - 1; lastRow >= 0; --lastRow)if(pRows[lastRow])break;
   
   for(i = firstRow; i <= lastRow; i += period){
      for(j = width - 1; j >= 0; --j){
         if((pRows[i] >> j) & 1) out += sprintf(out, "o");
         else out += sprintf(out, ".");
      }
      if(sp[P_SYMMETRY] != SYM_ASYM){
         if(sp[P_SYMMETRY] == SYM_GUTTER) out += sprintf(out, ".");
         if(sp[P_SYMMETRY] != SYM_ODD){
            if (pRows[i] & 1) out += sprintf(out, "o");
            else out += sprintf(out, ".");
         }
         for(j = 1; j < width; ++j){
            if((pRows[i] >> j) & 1) out += sprintf(out, "o");
            else out += sprintf(out, ".");
         }
      }
      out += sprintf(out, "\n");
   }
   out += sprintf(out, "Length: %d\n", lastRow - 2 * period + 1);
}

void printPattern(){
   printf("%s", buf);
   fflush(stdout);
}

int lookAhead(int a){
   uint32_t ri11, ri12, ri13, ri22, ri23;  //indices: first number represents vertical offset, second number represents generational offset
   uint32_t rowSet11, rowSet12, rowSet13, rowSet22, rowSet23, rowSet33;
   uint32_t riStart11, riStart12, riStart13, riStart22, riStart23;
   uint32_t numRows11, numRows12, numRows13, numRows22, numRows23;
   uint32_t row11, row12, row13, row22, row23;
   
   rowSet11 = (pRows[a - sp[P_PERIOD] - fwdOff[phase]] << (2 * sp[P_WIDTH]))
             +(pRows[a - fwdOff[phase]] << sp[P_WIDTH])
             + pRows[a];
   riStart11 = gInd[rowSet11];
   numRows11 = gInd[rowSet11 + 1] - riStart11;
   if(!numRows11) return 0;
   
   rowSet12 = (pRows[a - sp[P_PERIOD] - doubleOff[phase]] << (2 * sp[P_WIDTH]))
             +(pRows[a - doubleOff[phase]] << sp[P_WIDTH])
             + pRows[a - fwdOff[phase]];
   riStart12 = gInd[rowSet12];
   numRows12 = gInd[rowSet12 + 1] - riStart12;
   
   if(tripleOff[phase] >= sp[P_PERIOD]){
      riStart13 = pInd[a + sp[P_PERIOD] - tripleOff[phase]] + pRemain[a + sp[P_PERIOD] - tripleOff[phase]];
      numRows13 = 1;
   }
   else{
      rowSet13 = (pRows[a - sp[P_PERIOD] - tripleOff[phase]] << (2 * sp[P_WIDTH]))
                +(pRows[a - tripleOff[phase]] << sp[P_WIDTH])
                + pRows[a - doubleOff[phase]];
      riStart13 = gInd[rowSet13];
      numRows13 = gInd[rowSet13 + 1] - riStart13;
   }
   
   for(ri11 = 0; ri11 < numRows11; ++ri11){
      row11 = gRows[ri11 + riStart11];
      for(ri12 = 0; ri12 < numRows12; ++ri12){
         row12 = gRows[ri12 + riStart12];
         rowSet22 = (pRows[a - doubleOff[phase]] << (2 * sp[P_WIDTH]))
                   +(row12 << sp[P_WIDTH])
                   + row11;
         riStart22 = gInd[rowSet22];
         numRows22 = gInd[rowSet22 + 1] - riStart22;
         if(!numRows22) continue;
         
         for(ri13 = 0; ri13 < numRows13; ++ri13){
            row13 = gRows[ri13 + riStart13];
            rowSet23 = (pRows[a - tripleOff[phase]] << (2 * sp[P_WIDTH]))
                      +(row13 << sp[P_WIDTH])
                      + row12;
            riStart23 = gInd[rowSet23];
            numRows23 = gInd[rowSet23 + 1] - riStart23;
            if(!numRows23) continue;
            
            for(ri22 = 0; ri22 < numRows22; ++ri22){
               row22 = gRows[ri22 + riStart22];
               for(ri23 = 0; ri23 < numRows23; ++ri23){
                  row23 = gRows[ri23 + riStart23];
                  rowSet33 = (row13 << (2 * sp[P_WIDTH]))
                            +(row23 << sp[P_WIDTH])
                            + row22;
                  if(gInd[rowSet33] != gInd[rowSet33 + 1]) return 1;
               }
            }
         }
      }
   }
   return 0;
}

int dumpNum = 1;
char dumpFile[12];
#define DUMPROOT "dump"
int dumpFlag = 0; /* Dump status flags, possible values follow */
#define DUMPPENDING (1)
#define DUMPFAILURE (2)
#define DUMPSUCCESS (3)

int dumpandexit = 0;

FILE * openDumpFile(){
    FILE * fp;

    while (dumpNum < 10000)
    {
        sprintf(dumpFile,"%s%04d",DUMPROOT,dumpNum++);
        if((fp=fopen(dumpFile,"r")))
            fclose(fp);
        else
            return fopen(dumpFile,"w");
    }
    return (FILE *) 0;
}

void dumpState(int v){ // v = rowNum
    FILE * fp;
    int i;
    dumpFlag = DUMPFAILURE;
    if (!(fp = openDumpFile())) return;
    fprintf(fp,"%lu\n",FILEVERSION);
    for (i = 0; i < NUM_PARAMS; i++)
       fprintf(fp,"%d\n",sp[i]);
    fprintf(fp,"%d\n",firstFull);
    fprintf(fp,"%d\n",shipNum);
    for (i = 1; i <= shipNum; i++)
       fprintf(fp,"%u\n",lastNonempty[i]);
    fprintf(fp,"%d\n",v);
    for (i = 0; i < 2 * period; i++)
       fprintf(fp,"%lu\n",(unsigned long) pRows[i]);
    for (i = 2 * period; i <= v; i++){
       fprintf(fp,"%lu\n",(unsigned long) pRows[i]);
       fprintf(fp,"%lu\n",(unsigned long) pInd[i]);
       fprintf(fp,"%lu\n",(unsigned long) pRemain[i]);
    }
    fclose(fp);
    dumpFlag = DUMPSUCCESS;
}

int checkInteract(int a){
   int i;
   for(i = a - period; i > a - 2*period; --i){
      if(ev2Rows[(pRows[i] << width) + pRows[i + period]] != pRows[i + backOff[i % period]]) return 1;
   }
   return 0;
}

void search(){
   uint32_t currRow = rowNum;    // currRow == index of current row
   uint32_t newRowSet;           // used when determining the next row to be added
   int j;
   unsigned long long calcs, lastLong;
   int noship = 0;
   int totalShips = 0;
   calcs = 0;                    // calcs == "calculations" == number of times through the main loop
   uint32_t longest = 0;         // length of the longest partial seen so far
   lastLong = 0;                 // number of calculations at which longest was updated
   int buffFlag = 0;
   double ms = get_cpu_time();
   phase = currRow % period;
   for(;;){
      ++calcs;
      if(!(calcs & dumpPeriod)){
         dumpState(currRow);
         if(dumpFlag == DUMPSUCCESS) printf("State dumped to file %s%04d\n",DUMPROOT,dumpNum - 1);
         else printf("Dump failed\n");
         fflush(stdout);
      }
      if(currRow > longest || !(calcs & 0xffffff)){
         if(currRow > longest){
            buffPattern(currRow);
            longest = currRow;
            buffFlag = 1;
            lastLong = calcs;
         }
         if((buffFlag && calcs - lastLong > 0xffffff) || !(calcs & 0xffffffff)){
            if(!(calcs & 0xffffffff)) buffPattern(currRow);
            printPattern();
            printInfo(currRow,calcs,get_cpu_time()-ms);
            buffFlag = 0;
         }
      }
      if(!pRemain[currRow]){
         if(shipNum && lastNonempty[shipNum] == currRow) --shipNum;
         --currRow;
         if(phase == 0) phase = period;
         --phase;
         if(sp[P_FULL_PERIOD] && firstFull == currRow) firstFull = 0;
         if(currRow < 2 * sp[P_PERIOD]){
            printPattern();
            if(totalShips == 1)printf("Search complete: 1 spaceship found.\n");
            else printf("Search complete: %d spaceships found.\n",totalShips);
            printInfo(-1,calcs,get_cpu_time() - ms);
            return;
         }
         continue;
      }
      --pRemain[currRow];
      pRows[currRow] = gRows[pInd[currRow] + pRemain[currRow]];
      if(sp[P_MAX_LENGTH] && currRow > sp[P_MAX_LENGTH] + 2 * period - 1 && pRows[currRow] != 0) continue;  //back up if length exceeds max length
      if(sp[P_FULL_PERIOD] && currRow > sp[P_FULL_PERIOD] && !firstFull && pRows[currRow]) continue;        //back up if not full period by certain length
      if(sp[P_FULL_WIDTH] && (pRows[currRow] & fpBitmask)){
         if(equivRow[phase] < 0 && pRows[currRow] != pRows[currRow + equivRow[phase]]){
            if(!twoSubPeriods || (equivRow2[phase] < 0 && pRows[currRow] != pRows[currRow + equivRow2[phase]])) continue;
         }
      }
      if(shipNum && currRow == lastNonempty[shipNum] + 2*period && !checkInteract(currRow)) continue;       //back up if new rows don't interact with ship
      if(!lookAhead(currRow))continue;
      if(sp[P_FULL_PERIOD] && !firstFull){
         if(equivRow[phase] < 0 && pRows[currRow] != pRows[currRow + equivRow[phase]]){
            if(!twoSubPeriods || (equivRow2[phase] < 0 && pRows[currRow] != pRows[currRow + equivRow2[phase]])) firstFull = currRow;
         }
      }
      ++currRow;
      ++phase;
      if(phase == period) phase = 0;
      if(currRow > sp[P_DEPTH_LIMIT]){
         noship = 0;
         for(j = 1; j <= 2 * period; ++j) noship |= pRows[currRow-j];
         if(!noship){
            if(!sp[P_FULL_PERIOD] || firstFull){
               printf("\n");
               printPattern();
               ++totalShips;
               printf("Spaceship found. (%d)\n\n",totalShips);
               printInfo(currRow,calcs,get_cpu_time() - ms);
               --sp[P_NUM_SHIPS];
            }
            ++shipNum;
            if(sp[P_NUM_SHIPS] == 0){
               if(totalShips == 1)printf("Search terminated: spaceship found.\n");
               else printf("Search terminated: %d spaceships found.\n",totalShips);
               return;
            }
            for(lastNonempty[shipNum] = currRow - 1; lastNonempty[shipNum] >= 0; --lastNonempty[shipNum]) if(pRows[lastNonempty[shipNum]]) break;
            currRow = lastNonempty[shipNum] + 2 * period;
            phase = currRow % period;
            longest = lastNonempty[shipNum];
            continue;
         }
         else{
            printPattern();
            printf("Search terminated: depth limit reached.\n");
            printf("Depth: %d\n", currRow - 2 * period);
            if(totalShips == 1)printf("1 spaceship found.\n");
            else printf("%d spaceships found.\n",totalShips);
         }
         printInfo(currRow,calcs,get_cpu_time() - ms);
         return;
      }
      newRowSet = (pRows[currRow - 2 * period] << (2 * sp[P_WIDTH]))
                 +(pRows[currRow - period] << sp[P_WIDTH])
                 + pRows[currRow - period + backOff[phase]];
      pRemain[currRow] = gInd[newRowSet + 1] - gInd[newRowSet];
      pInd[currRow] = gInd[newRowSet];
   }
}

char * loadFile;

void loadFail(){
   printf("Load from file %s failed\n",loadFile);
   exit(1);
}

signed int loadInt(FILE *fp){
   signed int v;
   if (fscanf(fp,"%d\n",&v) != 1) loadFail();
   return v;
}

unsigned long loadUL(FILE *fp){
   unsigned long v;
   if (fscanf(fp,"%lu\n",&v) != 1) loadFail();
   return v;
}

void loadState(char * cmd, char * file){
   FILE * fp;
   int i;
   
   printf("Loading search state from %s\n",file);
   
   loadFile = file;
   fp = fopen(loadFile, "r");
   if (!fp) loadFail();
   if (loadUL(fp) != FILEVERSION)
   {
      printf("Incompatible file version\n");
      exit(1);
   }
   
   /* Load parameters and set stuff that can be derived from them */
   for (i = 0; i < NUM_PARAMS; i++)
      sp[i] = loadInt(fp);

   firstFull = loadInt(fp);
   shipNum = loadInt(fp);
   lastNonempty = malloc(sizeof(int) * (sp[P_DEPTH_LIMIT]/10));
   for (i = 1; i <= shipNum; i++)
      lastNonempty[i] = loadUL(fp);
   rowNum = loadInt(fp);
   
   if(sp[P_DUMP] > 0){
      if(sp[P_DUMP] < MIN_DUMP) sp[P_DUMP] = MIN_DUMP;
      dumpPeriod = ((long long)1 << sp[P_DUMP]) - 1;
   }
   
   rule = sp[P_RULE];
   width = sp[P_WIDTH];
   period = sp[P_PERIOD];
   offset = sp[P_OFFSET];
   if(gcd(period,offset) == 1) sp[P_FULL_PERIOD] = 0;
   if(sp[P_FULL_WIDTH] > sp[P_WIDTH]) sp[P_FULL_WIDTH] = 0;
   if(sp[P_FULL_WIDTH] && sp[P_FULL_WIDTH] < sp[P_WIDTH]){
      for(i = sp[P_FULL_WIDTH]; i < sp[P_WIDTH]; ++i){
         fpBitmask |= (1 << i);
      }
   }
   
   pRows = malloc(sp[P_DEPTH_LIMIT] * 2);
   pInd = malloc(sp[P_DEPTH_LIMIT] * 4);
   pRemain = malloc(sp[P_DEPTH_LIMIT] * 4);
   
   for (i = 0; i < 2 * period; i++)
      pRows[i] = (uint16_t) loadUL(fp);
   for (i = 2 * period; i <= rowNum; i++){
      pRows[i]   = (uint16_t) loadUL(fp);
      pInd[i]    = (uint32_t) loadUL(fp);
      pRemain[i] = (uint32_t) loadUL(fp);
   }
   fclose(fp);
   
   if(!strcmp(cmd,"p") || !strcmp(cmd,"P")){
      buffPattern(rowNum);
      printPattern();
      exit(0);
   }
}

void loadInitRows(char * file){
   FILE * fp;
   int i,j;
   char rowStr[MAXWIDTH];
   
   loadFile = file;
   fp = fopen(loadFile, "r");
   if (!fp) loadFail();
   
   for(i = 0; i < 2 * period; i++){
      fscanf(fp,"%s",rowStr);
      for(j = 0; j < width; j++){
         pRows[i] |= ((rowStr[width - j - 1] == '.') ? 0:1) << j;
      }
   }
   fclose(fp);
}

void initializeSearch(char * file){
   int i;
   if(sp[P_DUMP] > 0){
      if(sp[P_DUMP] < MIN_DUMP) sp[P_DUMP] = MIN_DUMP;
      dumpPeriod = ((long long)1 << sp[P_DUMP]) - 1;
   }
   rule = sp[P_RULE];
   width = sp[P_WIDTH];
   period = sp[P_PERIOD];
   offset = sp[P_OFFSET];
   if(sp[P_MAX_LENGTH]) sp[P_DEPTH_LIMIT] = sp[P_MAX_LENGTH] + 2 * period;
   sp[P_DEPTH_LIMIT] += 2 * period;
   if(sp[P_FULL_PERIOD]) sp[P_FULL_PERIOD] += 2 * period - 1;
   if(gcd(period,offset) == 1) sp[P_FULL_PERIOD] = 0;
   if(sp[P_FULL_WIDTH] > sp[P_WIDTH]) sp[P_FULL_WIDTH] = 0;
   if(sp[P_FULL_WIDTH] && sp[P_FULL_WIDTH] < sp[P_WIDTH]){
      for(i = sp[P_FULL_WIDTH]; i < sp[P_WIDTH]; ++i){
         fpBitmask |= (1 << i);
      }
   }
   
   pRows = malloc(sp[P_DEPTH_LIMIT] * 2);
   pInd = malloc(sp[P_DEPTH_LIMIT] * 4);
   pRemain = malloc(sp[P_DEPTH_LIMIT] * 4);
   lastNonempty = malloc(sizeof(int) * (sp[P_DEPTH_LIMIT]/10));
   rowNum = 2 * period;
   for(i = 0; i < 2 * period; i++)pRows[i] = 0;
   if(sp[P_INIT_ROWS]) loadInitRows(file);
}

void echoParams(){
   int i,j;
   printf("Rule: B");
   for(i = 0; i < 9; i++){
      if(rule & (1 << i)) printf("%d",i);
   }
   printf("/S");
   for(i = 9; i < 18; i++){
      if(rule & (1 << i)) printf("%d",i - 9);
   }
   printf("\n");
   printf("Period: %d\n",sp[P_PERIOD]);
   printf("Offset: %d\n",sp[P_OFFSET]);
   printf("Width:  %d\n",sp[P_WIDTH]);
   if(sp[P_SYMMETRY] == SYM_ASYM) printf("Symmetry: asymmetric\n");
   else if(sp[P_SYMMETRY] == SYM_ODD) printf("Symmetry: odd\n");
   else if(sp[P_SYMMETRY] == SYM_EVEN) printf("Symmetry: even\n");
   else if(sp[P_SYMMETRY] == SYM_GUTTER) printf("Symmetry: gutter\n");
   if(sp[P_MAX_LENGTH]) printf("Max length: %d\n",sp[P_MAX_LENGTH]);
   else printf("Depth limit: %d\n",sp[P_DEPTH_LIMIT] - 2 * period);
   if(sp[P_FULL_PERIOD]) printf("Full period by depth %d\n",sp[P_FULL_PERIOD] - 2 * period + 1);
   if(sp[P_FULL_WIDTH]) printf("Full period width: %d\n",sp[P_FULL_WIDTH]);
   if(sp[P_NUM_SHIPS] == 1) printf("Stop search if a ship is found.\n");
   else printf("Stop search if %d ships are found.\n",sp[P_NUM_SHIPS]);
   if(sp[P_DUMP])printf("Dump period: 2^%d\n",sp[P_DUMP]);
   if(!sp[P_REORDER]) printf("Use naive search order.\n");
   if(sp[P_INIT_ROWS]){
      printf("Initial rows:\n");
      for(i = 0; i < 2 * period; i++){
         for(j = width - 1; j >= 0; j--) printf("%c",(pRows[i] & (1 << j)) ? 'o':'.');
         printf("\n");
      }
   }
}

void usage(){
   printf("%s\n",BANNER);
   printf("\n");
   printf("Usage: \"zfind options\"\n");
   printf("  e.g. \"zfind B3/S23 p3 k1 w6 v\" searches Life (rule B3/S23) for\n");
   printf("  c/3 orthogonal spaceships with even bilateral symmetry and a\n");
   printf("  search width of 6 (full width 12).\n");
   printf("\n");
   printf("Available options:\n");
   printf("  bNN/sNN searches for spaceships in the specified rule (default: b3/s23)\n");
   printf("\n");
   printf("  pNN  searches for spaceships with period NN\n");
   printf("  kNN  searches for spaceships that travel NN cells every period\n");
   printf("  wNN  searches for spaceships with search width NN\n");
   printf("       (full width depends on symmetry type)\n");
   printf("\n");
   printf("  lNN  terminates the search if it reaches a depth of NN (default: %d)\n",DEFAULT_DEPTH_LIMIT);
   printf("  mNN  disallows spaceships longer than a depth of NN\n");
   printf("       (the spaceship length is approximately depth/period)\n");
   printf("  fNN  disallows spaceships that do not have the full period by a depth of NN\n");
   printf("  tNN  disallows full-period rows of width greater than NN\n");
   printf("  sNN  terminates the search if NN spaceships are found (default: 1)\n");
   printf("\n");
   printf("  dNN  dumps the search state every 2^NN calculations (minimum: %d)\n",MIN_DUMP);
   printf("  j    dumps the state at start of search\n");
   printf("\n");
   printf("  a    searches for asymmetric spaceships\n");
   printf("  u    searches for odd bilaterally symmetric spaceships\n");
   printf("  v    searches for even bilaterally symmetric spaceships\n");
   printf("  g    searches for symmetric spaceships with gutters (empty center column)\n");
   printf("\n");
   printf("  o    uses naive search order (search will take longer when no ships exist)\n");
   printf("\n");
   printf("  e FF uses rows in the file FF as the initial rows for the search\n");
   printf("       (use the companion Golly python script to easily generate the\n");
   printf("       initial row file)\n");
   printf("\n");
   printf("\"zfind command file\" reloads the state from the specified file\n");
   printf("and performs the command. Available commands: \n");
   printf("  s    resumes search from the loaded state\n");
   printf("  p    outputs the pattern representing the loaded state\n");
}

int main(int argc, char *argv[]){
   sp[P_RULE] = 6152;         //first 9 bits represent births; next 9 bits represent survivals
   sp[P_WIDTH] = 0;
   sp[P_PERIOD] = 0;
   sp[P_OFFSET] = 0;
   sp[P_DEPTH_LIMIT] = DEFAULT_DEPTH_LIMIT;
   sp[P_SYMMETRY] = 0;
   sp[P_MAX_LENGTH] = 0;
   sp[P_INIT_ROWS] = 0;
   sp[P_FULL_PERIOD] = 0;
   sp[P_NUM_SHIPS] = 1;
   sp[P_FULL_WIDTH] = 0;
   sp[P_REORDER] = 1;
   sp[P_DUMP] = 0;
   loadDumpFlag = 0;
   dumpPeriod = 0xffffffffffffffff;  // default dump period is 2^64, so the state will never be dumped
   int dumpandexit = 0;
   int skipNext = 0;
   int div1,div2;
   int s;
   if(argc == 2 && !strcmp(argv[1],"c")){
      usage();
      return 0;
   }
   if(argc == 3 && (!strcmp(argv[1],"s") || !strcmp(argv[1],"S") || !strcmp(argv[1],"p") || !strcmp(argv[1],"P"))) loadDumpFlag = 1;
   else{
      for(s = 1; s < argc; s++){    //read input parameters
         if(skipNext){
            skipNext = 0;
            continue;
         }
         switch(argv[s][0]){
            case 'b': case 'B':     //read rule
               sp[P_RULE] = 0;
               int sshift = 0;
               int i;
               for(i = 1; i < 100; i++){
                  int rnum = argv[s][i];
                  if(!rnum)break;
                  if(rnum == 's' || rnum == 'S')sshift = 9;
                  if(rnum >= '0' && rnum <= '8')sp[P_RULE] += 1 << (sshift + rnum - '0');
               }
            break;
            case 'w': case 'W': sscanf(&argv[s][1], "%d", &sp[P_WIDTH]); break;
            case 'p': case 'P': sscanf(&argv[s][1], "%d", &sp[P_PERIOD]); break;
            case 'k': case 'K': sscanf(&argv[s][1], "%d", &sp[P_OFFSET]); break;
            case 'l': case 'L': sscanf(&argv[s][1], "%d", &sp[P_DEPTH_LIMIT]); break;
            case 'u': case 'U': sp[P_SYMMETRY] = SYM_ODD; break;
            case 'v': case 'V': sp[P_SYMMETRY] = SYM_EVEN; break;
            case 'a': case 'A': sp[P_SYMMETRY] = SYM_ASYM; break;
            case 'g': case 'G': sp[P_SYMMETRY] = SYM_GUTTER; break;
            case 'm': case 'M': sscanf(&argv[s][1], "%d", &sp[P_MAX_LENGTH]); break;
            case 'd': case 'D': sscanf(&argv[s][1], "%d", &sp[P_DUMP]); break;
            case 'j': case 'J': dumpandexit = 1; break;
            case 'e': case 'E': sp[P_INIT_ROWS] = s + 1; skipNext = 1; break;
            case 'f': case 'F': sscanf(&argv[s][1], "%d", &sp[P_FULL_PERIOD]); break;
            case 's': case 'S': sscanf(&argv[s][1], "%d", &sp[P_NUM_SHIPS]); break;
            case 't': case 'T': sscanf(&argv[s][1], "%d", &sp[P_FULL_WIDTH]); break;
            case 'o': case 'O': sp[P_REORDER] = 0; break;
         }
      }
   }
   if(loadDumpFlag) loadState(argv[1],argv[2]);     //load search state from file
   else initializeSearch(argv[sp[P_INIT_ROWS]]);    //initialize search based on input parameters
   if(!sp[P_WIDTH] || !sp[P_PERIOD] || !sp[P_OFFSET] || !sp[P_SYMMETRY]){
      printf("You must specify a width, period, offset, and symmetry type.\n");
      printf("For command line options, type 'zfind c'.\n");
      return 0;
   }
   echoParams();
   makePhases();                    //make phase tables for determining successor row indices
   if(gcd(period,offset) > 1){      //make phase tables for determining equivalent subperiodic rows
      div1 = smallestDivisor(gcd(period,offset));
      makeEqRows(period / div1,1);
      div2 = gcd(period,offset);
      while(div2 % div1 == 0) div2 /= div1;
      if(div2 != 1){
         twoSubPeriods = 1;
         div2 = smallestDivisor(div2);
         makeEqRows(period / div2,2);
      }
   }
   makeTables();                    //make lookup tables for determining successor rows
   if(!loadDumpFlag){               //these initialization steps must be performed after makeTables()
      pRemain[2 * period] = gInd[1] - gInd[0] - 1;
      pInd[2 * period] = gInd[0];
      if(sp[P_INIT_ROWS]){
         s = (pRows[0] << (2 * width)) + (pRows[period] << width) + pRows[period + backOff[0]];
         pRemain[2 * period] = gInd[s + 1] - gInd[s];
         pInd[2 * period] = gInd[s];
      }
   }
   if(dumpandexit){
      dumpState(rowNum);
      if (dumpFlag == DUMPSUCCESS) printf("State dumped to file %s%04d\n",DUMPROOT,dumpNum - 1);
      else printf("Dump failed\n");
      return 0;
   }
   buf = malloc((2*sp[P_WIDTH] + 4) * sp[P_DEPTH_LIMIT]);  // I think this gives more than enough space
   buf[0] = '\0';
   printf("Starting search\n");
   search();
   return 0;
}

/* Host test for main/stand.c (bot stand judge).
 *   cc -Wall -Wextra -Werror -fsanitize=address,undefined -Imain tools/test_stand.c main/stand.c -o /tmp/test_stand && /tmp/test_stand */
#include "stand.h"
#include <assert.h>
#include <stdio.h>
#define S 1000000LL
int main(void){
  char l[64]; stand_pet_t t[4];
  stand_reset();
  /* B214 strong every second for 100 s */
  for (int s=0;s<=100;s++){ stand_heard("Claude-B214",-50,s*S); stand_tick(s*S); }
  assert(stand_top(t,4,100*S)==1 && t[0].on && stand_current_s(&t[0],100*S)==100);
  /* far pet never counts */
  for (int s=0;s<=100;s++){ stand_heard("Claude-5610",-80,s*S); stand_tick(s*S); }
  /* B214 away 20 s (< grace) then back: same visit */
  for (int s=121;s<=150;s++){ stand_heard("Claude-B214",-50,s*S); stand_tick(s*S); }
  stand_top(t,4,150*S); assert(t[0].on && stand_current_s(&t[0],150*S)==150);
  /* gone 40 s: visit ends, best 150 */
  stand_tick(181*S); stand_top(t,4,181*S); assert(!t[0].on && t[0].best_us==150*S);
  /* brush past 5 s: no visit */
  for (int s=200;s<=205;s++){ stand_heard("Claude-AAAA",-45,s*S); stand_tick(s*S);} stand_tick(240*S);
  int n=stand_top(t,4,240*S); assert(n==3); assert(stand_score_s(&t[0],240*S)==150);
  for(int i=0;i<n;i++) printf("%s score=%d on=%d rssi=%d\n",t[i].name,stand_score_s(&t[i],240*S),t[i].on,t[i].rssi);
  assert(stand_line(l,sizeof l,240*S)); printf("line: %s\n",l);
  /* one loud packet among quiet ones does not start a visit */
  stand_reset(); for(int s=0;s<60;s++){ stand_heard("Claude-X",(s%10==0)?-40:-85,s*S); stand_tick(s*S);} stand_top(t,1,60*S); assert(!t[0].on);
  puts("ok"); return 0;}

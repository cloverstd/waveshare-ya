#include "../main/notifications.hpp"
#include <cassert>
#include <string>
using namespace ya;
int main() {
  NotificationLog log;
  for(int i=0;i<15;++i)log.append(std::to_string(i).c_str(),NoticeKind::SYSTEM,1,i*6000,0);
  assert(log.count==10 && log.entries[0].id==6 && log.unread()==10);
  log.read=log.entries[9].id;assert(log.unread()==0);
  log.append("14",NoticeKind::SYSTEM,1,85000,0);assert(log.count==10 && log.entries[9].repeats==2 && log.unread()==1);
  auto restored=log;assert(restored.count==10 && restored.entries[9].id==log.entries[9].id);
  assert(log.correct_time(1,90000,1800000000)); assert(log.entries[9].epoch==1799999995);
  log.append("unsynced",NoticeKind::SYSTEM,2,100,0);assert(!log.correct_time(3,200,1800000000));
  auto cutoff=log.next-1;auto saved=log; saved.erase_through(cutoff); // staged write fails: original remains
  assert(log.count==10 && saved.count==0);
  log.append("arrived during clear",NoticeKind::BLE,2,200,0);
  log.erase_through(cutoff);assert(log.count==1 && std::string(log.entries[0].text)=="arrived during clear");
  log.append(std::string(80,'x').append("中文中文中文中文中文中文中文中文中文中文中文中文中文中文中文中文").c_str(),NoticeKind::SYSTEM,2,6000,0);
  assert(strlen(log.entries[1].text)<160);
}

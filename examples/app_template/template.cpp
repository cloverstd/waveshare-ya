#include "template.hpp"
namespace ya::example {
static lv_timer_t* timer=nullptr;
static lv_obj_t* value=nullptr;
static void refresh(lv_timer_t*){auto s=snapshot();lv_label_set_text_fmt(value,"Battery: %d%%",s.battery.valid?s.battery.percent:-1);}
static void create(lv_obj_t* parent){value=lv_label_create(parent);lv_obj_set_width(value,LV_PCT(100));refresh(nullptr);}
static void enter(){timer=lv_timer_create(refresh,1000,nullptr);}
static void leave(){if(timer){lv_timer_delete(timer);timer=nullptr;}}
static void destroy(){value=nullptr;}
const App descriptor={"example","示例","E",0,create,enter,leave,destroy};
}

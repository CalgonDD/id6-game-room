// HOST TEST DOUBLE ONLY. Never put this directory on the QNX include path.
#ifndef BMW_LIST_H
#define BMW_LIST_H
#include <assert.h>
#include <stdint.h>
#include <string.h>

extern uint64_t hmi_test_now;
struct HmiTestWrite { uint64_t at; unsigned model; };
extern HmiTestWrite hmi_test_writes[4096];
extern unsigned hmi_test_write_count;
namespace bmw {
class IDispatcher {};
class Bundle {};
struct RHMIInterfaceVersion { unsigned value; };
template <class T> class BasicString {
public:
    explicit BasicString(const T*) {}
};
struct ImageID { unsigned id,type; explicit ImageID(unsigned i):id(i),type(0){} };
class Parcel {
    unsigned values_[8],count_;
    mutable unsigned cursor_;
public:
    Parcel():count_(0),cursor_(0){memset(values_,0,sizeof values_);}
    explicit Parcel(bool value):count_(1),cursor_(0){memset(values_,0,sizeof values_);values_[0]=value?1u:0u;}
    void writeUInt32(unsigned value){assert(count_<8);values_[count_++]=value;}
    bool readUInt32(unsigned& value) const {
        if(cursor_>=count_)return false;
        value=values_[cursor_++];return true;
    }
    bool readBool(bool& value) const {
        unsigned raw;if(!readUInt32(raw))return false;value=raw!=0;return true;
    }
    const unsigned char* rawData() const {return reinterpret_cast<const unsigned char*>(values_);}
    unsigned rawSize() const {return count_*4u;}
    unsigned rawPos() const {return cursor_*4u;}
};
class Message {
public:
    unsigned id; Parcel data;
    Message():id(0){}
    Message(unsigned i,const Parcel& p):id(i),data(p){}
};
template<class R,class A1=void,class A2=void,class A3=void,class A4=void> class Delegate {
public:
    void* m_obj;R (*m_exec)(void*,A1);
    template<class C,R(C::*Method)(A1)> static R execImpl(void* instance,A1 argument) {
        return (static_cast<C*>(instance)->*Method)(argument);
    }
};
class PluginApp {
    struct Handler {unsigned state,event;Delegate<bool,const Parcel&> delegate;} handlers_[128];
    struct Pending {uint64_t due;Message message;} timers_[64];
    unsigned handler_count_,timer_count_;
    void write(unsigned model){
        assert(hmi_test_write_count<4096);
        hmi_test_writes[hmi_test_write_count].at=hmi_test_now;
        hmi_test_writes[hmi_test_write_count++].model=model;
    }
public:
    PluginApp(IDispatcher&,BasicString<char>,unsigned,unsigned,RHMIInterfaceVersion,const Bundle&,const Bundle&)
        :handler_count_(0),timer_count_(0){}
    virtual ~PluginApp(){}
    virtual bool onInit(){return true;}
    virtual bool onFinalize(){return true;}
    virtual bool invokeDataUpdate(unsigned,const Parcel&){return true;}
    void updateModel(unsigned model,const BasicString<char>&){write(model);}
    void updateModel(unsigned model,const ImageID&){write(model);}
    void addHandler(unsigned state,unsigned event,const Delegate<bool,const Parcel&>& delegate){
        assert(handler_count_<128);Handler& h=handlers_[handler_count_++];
        h.state=state;h.event=event;h.delegate=delegate;
    }
    void addHandler(unsigned action,const Delegate<bool,const Parcel&>& delegate){addHandler(0,action,delegate);}
    void post(const Message& message,unsigned delay){
        assert(timer_count_<64);Pending& t=timers_[timer_count_++];t.due=hmi_test_now+delay;t.message=message;
    }
    void event(unsigned state,unsigned event,bool value){
        for(unsigned i=0;i<handler_count_;++i)if(handlers_[i].state==state && handlers_[i].event==event){
            Parcel p(value);assert(handlers_[i].delegate.m_exec(handlers_[i].delegate.m_obj,p));return;
        }
        assert(!"unregistered event/action");
    }
    // Like event(), but with any payload and returning the handler's answer (a page returns false for
    // page events it does not understand). -1: nothing registered.
    int deliver(unsigned state,unsigned event,const Parcel& p){
        for(unsigned i=0;i<handler_count_;++i)if(handlers_[i].state==state && handlers_[i].event==event)
            return handlers_[i].delegate.m_exec(handlers_[i].delegate.m_obj,p) ? 1 : 0;
        return -1;
    }
    void advance(uint64_t until){
        unsigned iterations=0;assert(until>=hmi_test_now);
        while(timer_count_){
            unsigned next=0;
            for(unsigned i=1;i<timer_count_;++i)if(timers_[i].due<timers_[next].due)next=i;
            if(timers_[next].due>until)break;
            assert(++iterations<10000);
            Pending timer=timers_[next];timers_[next]=timers_[--timer_count_];
            hmi_test_now=timer.due;unsigned key;assert(timer.message.data.readUInt32(key));
            assert(invokeDataUpdate(key,timer.message.data));
        }
        hmi_test_now=until;
    }
};
}
#endif

#include "simfil/intrusive_ptr.h"

#include <catch2/catch_test_macros.hpp>

using namespace simfil;

struct Stats
{
    unsigned dtor = 0;
};

struct MyType : ref_counted<MyType>
{
    Stats* s;

    MyType(Stats* s) : s(s) {}
    ~MyType() {++s->dtor;}
};

TEST_CASE("Intrusive Pointer", "[intrusive_ptr]") {
    Stats s;

    {
        auto ptr = make_intrusive<MyType>(&s);
        REQUIRE(ptr->refcount() == 1);

        intrusive_ptr<MyType> empty;
        {
            intrusive_ptr<const MyType> cpy(ptr);
            REQUIRE(ptr->refcount() == 2);
            empty = ptr;
            REQUIRE(cpy->refcount() == 3);
            (void)cpy;
        }

        REQUIRE(ptr->refcount() == 2);
    }

    REQUIRE(s.dtor == 1);
}

#include <HAL/IwmCommandSlots.h>
#include <assert.h>
#include <stdio.h>
#include <random>

using namespace IwmCommand;

static void transitions()
{
    Slots<4> slots;
    Token a = {}, b = {};
    int error = -1;
    assert(!slots.reserve(4, 1, true, a));
    assert(slots.reserve(0, 1, true, a));
    assert(!slots.reserve(0, 2, true, b));
    assert(!slots.accepts(0, 1));
    assert(!slots.abandon(a));
    assert(slots.publish(a));
    assert(!slots.publish(a));
    assert(!slots.cancel(a));
    assert(slots.acknowledge(0, 2, 0) == Ack::Ignored);
    assert(slots.acknowledge(4, 1, 0) == Ack::Ignored);
    assert(slots.acknowledge(0, 1, 9) == Ack::Wake);
    assert(slots.acknowledge(0, 1, 0) == Ack::Ignored);
    assert(!slots.reserve(0, 2, true, b));
    assert(slots.consume(a, error) && error == 9);
    assert(!slots.consume(a, error));
    assert(slots.reserve(0, 1, false, b));
    assert(slots.publish(b));
    assert(!slots.abandon(b));
    assert(slots.acknowledge(0, 1, 0) == Ack::Release);
    assert(slots.reserve(0, 1, true, b));
    assert(slots.cancel(b));
}

static void timeoutAndReset()
{
    Slots<1> slots;
    Token old = {}, current = {};
    int error = -1;
    assert(slots.reserve(0, 7, true, old));
    assert(slots.publish(old));
    assert(slots.abandon(old));
    assert(!slots.reserve(0, 7, true, current));
    assert(slots.acknowledge(0, 7, 0) == Ack::Release);
    assert(slots.reserve(0, 7, true, current));
    assert(!slots.publish(old) && !slots.cancel(old) && !slots.abandon(old));
    assert(slots.publish(current));
    old = current;
    slots.resetAfterDrain();
    assert(slots.state(old) == State::Free);
    assert(slots.reserve(0, 7, true, current));
    assert(current.serial != old.serial);
    assert(slots.publish(current));
    assert(!slots.consume(old, error));
    assert(slots.acknowledge(0, 7, 0) == Ack::Wake);
    assert(slots.consume(current, error) && error == 0);
}

static void randomized()
{
    std::mt19937 random(0x3166);
    for (unsigned trace = 0; trace < 20000; ++trace) {
        Slots<4> slots;
        State states[4] = {};
        bool sync[4] = {};
        Token tokens[4] = {};
        int errors[4] = {};
        for (unsigned step = 0; step < 128; ++step) {
            const auto index = random() % 4;
            int error = -1;
            switch (random() % 7) {
                case 0: {
                    bool synchronous = (random() & 1) != 0;
                    const bool ok = slots.reserve(index, index + 1, synchronous, tokens[index]);
                    assert(ok == (states[index] == State::Free));
                    if (ok) { states[index] = State::Reserved; sync[index] = synchronous; }
                    break;
                }
                case 1:
                    assert(slots.publish(tokens[index]) == (states[index] == State::Reserved));
                    if (states[index] == State::Reserved) states[index] = State::Pending;
                    break;
                case 2:
                    assert(slots.cancel(tokens[index]) == (states[index] == State::Reserved));
                    if (states[index] == State::Reserved) states[index] = State::Free;
                    break;
                case 3: {
                    const int status = random() % 3;
                    const bool pending = states[index] == State::Pending || states[index] == State::Abandoned;
                    const bool wake = states[index] == State::Pending && sync[index];
                    assert(slots.acknowledge(index, index + 1, status) ==
                           (pending ? (wake ? Ack::Wake : Ack::Release) : Ack::Ignored));
                    if (pending) { states[index] = wake ? State::Complete : State::Free; errors[index] = status; }
                    break;
                }
                case 4:
                    assert(slots.consume(tokens[index], error) == (states[index] == State::Complete));
                    if (states[index] == State::Complete) {
                        assert(error == errors[index]); states[index] = State::Free;
                    }
                    break;
                case 5: {
                    const bool pending = states[index] == State::Pending && sync[index];
                    assert(slots.abandon(tokens[index]) == pending);
                    if (pending) states[index] = State::Abandoned;
                    break;
                }
                case 6:
                    slots.resetAfterDrain();
                    for (auto &state : states) state = State::Free;
                    break;
            }
            for (unsigned i = 0; i < 4; ++i) assert(slots.state(tokens[i]) == states[i]);
        }
    }
}

int main()
{
    transitions();
    timeoutAndReset();
    randomized();
    puts("IWM command slots: focused tests and 2,560,000 transitions passed");
}

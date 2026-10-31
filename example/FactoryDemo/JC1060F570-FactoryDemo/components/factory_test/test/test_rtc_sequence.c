#include <assert.h>

#include "ft_rtc_sequence.h"

int main(void)
{
    ft_rtc_sequence_t s = {0};
    assert(ft_rtc_sequence_sample(&s, 0x58));
    assert(ft_rtc_sequence_sample(&s, 0x58));
    assert(s.transitions == 0); /* A stopped clock cannot pass. */
    assert(ft_rtc_sequence_sample(&s, 0x59));
    assert(ft_rtc_sequence_sample(&s, 0x00));
    assert(s.transitions == 2 && s.second == 0);
    assert(!ft_rtc_sequence_sample(&s, 0x02));
    assert(!ft_rtc_sequence_sample(&s, 0x6a));
    assert(!ft_rtc_sequence_sample(&s, 0x60));
    return 0;
}

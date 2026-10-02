#include <assert.h>
#include <stdio.h>
#include "nmea_editor/numeric_editor_validation.h"

int main(void)
{
    const int8_t clock_map[] = {0, 1, -1, 2, 3, -1, 4, 5, -2};
    const field_limit_t clock_limits[] = {{0,2,0,23}, {2,2,0,59}, {4,2,0,59}};
    assert(num_edit_fields_valid("235959", 6, clock_map, 9, clock_limits, 3));
    assert(num_edit_fields_valid("000000", 6, clock_map, 9, clock_limits, 3));
    /* A prefix can be acceptable while the unchanged suffix makes an invalid draft. */
    assert(!num_edit_fields_valid("295959", 6, clock_map, 9, clock_limits, 3));
    assert(!num_edit_fields_valid("236000", 6, clock_map, 9, clock_limits, 3));
    assert(!num_edit_fields_valid("235960", 6, clock_map, 9, clock_limits, 3));
    assert(!num_edit_fields_valid("23x959", 6, clock_map, 9, clock_limits, 3));
    assert(!num_edit_fields_valid("235959", 5, clock_map, 9, clock_limits, 3));
    const field_limit_t date_limits[] = {{0,2,1,31}, {2,2,1,12}, {4,2,0,99}};
    assert(num_edit_fields_valid("311299", 6, clock_map, 9, date_limits, 3));
    assert(!num_edit_fields_valid("001299", 6, clock_map, 9, date_limits, 3));
    assert(!num_edit_fields_valid("310099", 6, clock_map, 9, date_limits, 3));
    const int8_t lon_map[] = {0,1,2,-1,3,4,-1,6,7,8,9,-2};
    const field_limit_t lon_limits[] = {{0,3,0,180},{3,2,0,59},{6,4,0,9999}};
    assert(num_edit_fields_valid("18000.0000",10,lon_map,12,lon_limits,3));
    assert(!num_edit_fields_valid("18100.0000",10,lon_map,12,lon_limits,3));
    assert(!num_edit_fields_valid("17960.0000",10,lon_map,12,lon_limits,3));
    const field_limit_t malformed[] = {{0,6,0,9999}};
    assert(!num_edit_fields_valid("000000",6,clock_map,9,malformed,1));
    assert(!num_edit_fields_valid("000000",6,clock_map,9,NULL,1));
    puts("Numeric editor complete-draft bounds validation tests passed");
    return 0;
}

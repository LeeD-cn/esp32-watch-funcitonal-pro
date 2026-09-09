#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "qmc5883p.h"

#define PI 3.14159265358979323846f

static float angle_error(float actual, float expected)
{
    float error = fabsf(actual - expected);
    return error > 180.0f ? 360.0f - error : error;
}

static void expect_heading(float mag_x,
                           float mag_y,
                           float mag_z,
                           float gravity_x,
                           float gravity_y,
                           float gravity_z,
                           float expected)
{
    float heading = -1.0f;
    assert(qmc5883p_calc_tilt_compensated_heading_deg(
               mag_x, mag_y, mag_z,
               gravity_x, gravity_y, gravity_z,
               0.0f, &heading) == QMC5883P_OK);
    assert(angle_error(heading, expected) < 0.01f);
}

static void expect_pitch(float heading_deg, float pitch_deg)
{
    float heading = heading_deg * PI / 180.0f;
    float pitch = pitch_deg * PI / 180.0f;
    float north_on_forward = cosf(pitch) * cosf(heading) + 0.4f * sinf(pitch);
    float north_on_side = sinf(heading);
    float north_on_normal = sinf(pitch) * cosf(heading) - 0.4f * cosf(pitch);

    expect_heading(north_on_forward,
                   north_on_side,
                   north_on_normal,
                   sinf(pitch),
                   0.0f,
                   -cosf(pitch),
                   heading_deg);
}

static void expect_roll(float heading_deg, float roll_deg)
{
    float heading = heading_deg * PI / 180.0f;
    float roll = roll_deg * PI / 180.0f;
    float north_on_forward = cosf(heading);
    float north_on_side = cosf(roll) * sinf(heading) + 0.4f * sinf(roll);
    float north_on_normal = sinf(roll) * sinf(heading) - 0.4f * cosf(roll);

    expect_heading(north_on_forward,
                   north_on_side,
                   north_on_normal,
                   0.0f,
                   sinf(roll),
                   -cosf(roll),
                   heading_deg);
}

int main(void)
{
    float heading = 0.0f;

    expect_heading(1.0f, 0.0f, 0.4f, 0.0f, 0.0f, -1.0f, 0.0f);
    expect_heading(0.0f, 1.0f, 0.4f, 0.0f, 0.0f, -1.0f, 90.0f);
    expect_pitch(60.0f, 30.0f);
    expect_pitch(359.0f, -30.0f);
    expect_roll(225.0f, 30.0f);
    expect_roll(15.0f, -30.0f);

    assert(qmc5883p_calc_tilt_compensated_heading_deg(
               1.0f, 0.0f, 0.0f,
               0.0f, 0.0f, 0.0f,
               0.0f, &heading) == QMC5883P_ERR_INVALID_VECTOR);
    assert(qmc5883p_calc_tilt_compensated_heading_deg(
               0.0f, 0.0f, 1.0f,
               0.0f, 0.0f, 1.0f,
               0.0f, &heading) == QMC5883P_ERR_INVALID_VECTOR);

    puts("qmc5883p tilt-compensation tests passed");
    return 0;
}

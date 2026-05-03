/*
 *

 * From https://github.com/nicolausYes/easing-functions/blob/master/src/easing.cpp
 */
#ifndef CUPID_COMMON_EASING_H
#define CUPID_COMMON_EASING_H

#include "types.h"

#include <math.h>

#define EASING_PI 3.1415926545f

ALWAYS_INLINE_RELEASE float easing_in_sine(float t)    { return sinf(1.5707963f * t); }
ALWAYS_INLINE_RELEASE float easing_out_sine(float t)   { return 1.0f + sinf(1.5707963f * (t - 1.0f)); }
ALWAYS_INLINE_RELEASE float easing_inout_sine(float t) { return 0.5f * (1.0f + sinf(3.1415926f * (t - 0.5f))); }

ALWAYS_INLINE_RELEASE float easing_in_quad(float t)    { return t * t; }
ALWAYS_INLINE_RELEASE float easing_out_quad(float t)   { return t * (2.0f - t); }
ALWAYS_INLINE_RELEASE float easing_inout_quad(float t) { return t < 0.5f ? 2.0f * t * t : t * (4.0f - 2.0f * t) - 1.0f; }

ALWAYS_INLINE_RELEASE float easing_in_cubic(float t)   { return t * t * t; }
ALWAYS_INLINE_RELEASE float easing_out_cubic(float t)
{
  float u = t - 1.0f;
  return 1.0f + u * u * u;
}
ALWAYS_INLINE_RELEASE float easing_inout_cubic(float t)
{
  if (t < 0.5f)
    return 4.0f * t * t * t;
  float u = 2.0f * t - 2.0f;
  return 1.0f + 0.5f * u * u * u;
}

ALWAYS_INLINE_RELEASE float easing_in_quart(float t)
{
  float u = t * t;
  return u * u;
}
ALWAYS_INLINE_RELEASE float easing_out_quart(float t)
{
  float u = (t - 1.0f) * (t - 1.0f);
  return 1.0f - u * u;
}
ALWAYS_INLINE_RELEASE float easing_inout_quart(float t)
{
  if (t < 0.5f) {
    float u = t * t;
    return 8.0f * u * u;
  }
  float u = (t - 1.0f) * (t - 1.0f);
  return 1.0f - 8.0f * u * u;
}

ALWAYS_INLINE_RELEASE float easing_in_quint(float t)
{
  float t2 = t * t;
  return t * t2 * t2;
}
ALWAYS_INLINE_RELEASE float easing_out_quint(float t)
{
  float u = t - 1.0f;
  float t2 = u * u;
  return 1.0f + u * t2 * t2;
}
ALWAYS_INLINE_RELEASE float easing_inout_quint(float t)
{
  if (t < 0.5f) {
    float t2 = t * t;
    return 16.0f * t * t2 * t2;
  }
  float u = t - 1.0f;
  float t2 = u * u;
  return 1.0f + 16.0f * u * t2 * t2;
}

ALWAYS_INLINE_RELEASE float easing_in_expo(float t)    { return (powf(2.0f, 8.0f * t) - 1.0f) / 255.0f; }
ALWAYS_INLINE_RELEASE float easing_out_expo(float t)   { return 1.0f - powf(2.0f, -8.0f * t); }
ALWAYS_INLINE_RELEASE float easing_inout_expo(float t)
{
  if (t < 0.5f)
    return (powf(2.0f, 16.0f * t) - 1.0f) / 510.0f;
  return 1.0f - 0.5f * powf(2.0f, -16.0f * (t - 0.5f));
}

ALWAYS_INLINE_RELEASE float easing_in_circ(float t)    { return 1.0f - sqrtf(1.0f - t); }
ALWAYS_INLINE_RELEASE float easing_out_circ(float t)   { return sqrtf(t); }
ALWAYS_INLINE_RELEASE float easing_inout_circ(float t)
{
  if (t < 0.5f)
    return (1.0f - sqrtf(1.0f - 2.0f * t)) * 0.5f;
  return (1.0f + sqrtf(2.0f * t - 1.0f)) * 0.5f;
}

ALWAYS_INLINE_RELEASE float easing_in_back(float t)    { return t * t * (2.70158f * t - 1.70158f); }
ALWAYS_INLINE_RELEASE float easing_out_back(float t)
{
  float u = t - 1.0f;
  return 1.0f + u * u * (2.70158f * u + 1.70158f);
}
ALWAYS_INLINE_RELEASE float easing_inout_back(float t)
{
  if (t < 0.5f)
    return t * t * (7.0f * t - 2.5f) * 2.0f;
  float u = t - 1.0f;
  return 1.0f + u * u * 2.0f * (7.0f * u + 2.5f);
}

ALWAYS_INLINE_RELEASE float easing_in_elastic(float t)
{
  float t2 = t * t;
  return t2 * t2 * sinf(t * EASING_PI * 4.5f);
}
ALWAYS_INLINE_RELEASE float easing_out_elastic(float t)
{
  float u = (t - 1.0f) * (t - 1.0f);
  return 1.0f - u * u * cosf(t * EASING_PI * 4.5f);
}
ALWAYS_INLINE_RELEASE float easing_inout_elastic(float t)
{
  if (t < 0.45f) {
    float t2 = t * t;
    return 8.0f * t2 * t2 * sinf(t * EASING_PI * 9.0f);
  } else if (t < 0.55f) {
    return 0.5f + 0.75f * sinf(t * EASING_PI * 4.0f);
  }
  float u = (t - 1.0f) * (t - 1.0f);
  return 1.0f - 8.0f * u * u * sinf(t * EASING_PI * 9.0f);
}

ALWAYS_INLINE_RELEASE float easing_in_bounce(float t)
{
  return powf(2.0f, 6.0f * (t - 1.0f)) * fabsf(sinf(t * EASING_PI * 3.5f));
}
ALWAYS_INLINE_RELEASE float easing_out_bounce(float t)
{
  return 1.0f - powf(2.0f, -6.0f * t) * fabsf(cosf(t * EASING_PI * 3.5f));
}
ALWAYS_INLINE_RELEASE float easing_inout_bounce(float t)
{
  if (t < 0.5f)
    return 8.0f * powf(2.0f, 8.0f * (t - 1.0f)) * fabsf(sinf(t * EASING_PI * 7.0f));
  return 1.0f - 8.0f * powf(2.0f, -8.0f * t) * fabsf(sinf(t * EASING_PI * 7.0f));
}

#endif /* CUPID_COMMON_EASING_H */

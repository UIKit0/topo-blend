#pragma once
#include "Curve.h"

template <typename Real>
class Integrate1
{
public:
    // The last parameter is for user-defined data.
    typedef Real (*Function)(Real,void*);

    static Real RombergIntegral (int order, Real a, Real b, Function function, void* userData = 0);
    static Real GaussianQuadrature (Real a, Real b, Function function, void* userData = 0);
    static Real TrapezoidRule (int numSamples, Real a, Real b, Function function, void* userData = 0);
};

typedef Integrate1<float> Integrate1f;
typedef Integrate1<double> Integrate1d;

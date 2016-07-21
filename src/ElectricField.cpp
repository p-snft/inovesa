/******************************************************************************
 * Inovesa - Inovesa Numerical Optimized Vlasov-Equation Solver Application   *
 * Copyright (c) 2014-2016: Patrik Schönfeldt                                 *
 *                                                                            *
 * This file is part of Inovesa.                                              *
 * Inovesa is free software: you can redistribute it and/or modify            *
 * it under the terms of the GNU General Public License as published by       *
 * the Free Software Foundation, either version 3 of the License, or          *
 * (at your option) any later version.                                        *
 *                                                                            *
 * Inovesa is distributed in the hope that it will be useful,                 *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU General Public License for more details.                               *
 *                                                                            *
 * You should have received a copy of the GNU General Public License          *
 * along with Inovesa.  If not, see <http://www.gnu.org/licenses/>.           *
 ******************************************************************************/

#include "ElectricField.hpp"

vfps::ElectricField::ElectricField(PhaseSpace* ps,
                                   const Impedance* impedance,
                                   const meshaxis_t wakescalining) :
    _nmax(impedance->nFreqs()),
    _bpmeshcells(ps->nMeshCells(0)),
    _axis_freq(Ruler<frequency_t>(_nmax,0,
                                  1/(ps->getDelta(0)),
                                  physcons::c/ps->getScale(0))),
    // _axis_wake[_bpmeshcells] will be at position 0
    _axis_wake(Ruler<meshaxis_t>(2*_bpmeshcells,
                                 -ps->getDelta(0)*_bpmeshcells,
                                  ps->getDelta(0)*(_bpmeshcells-1),
                                 ps->getScale(0))),
    _phasespace(ps),
    _csrintensity(0),
    _csrspectrum(new csrpower_t[_nmax]),
    _impedance(impedance),
    _wakefunction(nullptr),
    _wakelosses(nullptr),
    _wakelosses_fft(nullptr),
    _wakepotential_complex(nullptr),
    _wakepotential_fft(nullptr),
    _wakepotential(wakescalining!=0?new meshaxis_t[_bpmeshcells]:nullptr),
    _fft_wakelosses(nullptr),
    #ifdef INOVESA_USE_CLFFT
    _wakescaling(OCLH::active?
                   2*wakescalining*_axis_freq.delta()*_axis_wake.delta()*_nmax:
                   2*wakescalining*_axis_freq.delta()*_axis_wake.delta())
    #else
    _wakescaling(2*wakescalining*_axis_freq.delta()*_axis_wake.delta())
    #endif // INOVESA_USE_CLFFT
{
    #ifdef INOVESA_USE_CLFFT
    if (OCLH::active) {
        _bp_padded = new integral_t[_nmax];
        std::fill_n(_bp_padded,_nmax,0);
        _bp_padded_buf = cl::Buffer(OCLH::context,
                                      CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
                                      sizeof(integral_t)*_nmax,_bp_padded);
        _formfactor = new impedance_t[_nmax];
        _formfactor_buf = cl::Buffer(OCLH::context,
                                       CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
                                       sizeof(impedance_t)*_nmax,_formfactor);
        clfftCreateDefaultPlan(&_clfft_bunchprofile,
                               OCLH::context(),CLFFT_1D,&_nmax);
        clfftSetPlanPrecision(_clfft_bunchprofile,CLFFT_SINGLE);
        clfftSetLayout(_clfft_bunchprofile, CLFFT_REAL, CLFFT_HERMITIAN_INTERLEAVED);
        clfftSetResultLocation(_clfft_bunchprofile, CLFFT_OUTOFPLACE);
        clfftBakePlan(_clfft_bunchprofile,1,&OCLH::queue(), nullptr, nullptr);

        std::string cl_code_padbp = R"(
            __kernel void pad_bp(__global float* bp_padded,
                                 const ulong paddedsize,
                                 const uint bpmeshcells,
                                 const __global float* bp)
            {
                const uint g = get_global_id(0);
                const uint b = (g+bpmeshcells/2)%bpmeshcells;
                const uint p = (b+paddedsize-bpmeshcells/2)%paddedsize;
                bp_padded[p] = bp[b];
            }
            )";

        _clProgPadBP = OCLH::prepareCLProg(cl_code_padbp);
        _clKernPadBP = cl::Kernel(_clProgPadBP, "pad_bp");
        _clKernPadBP.setArg(0, _bp_padded_buf);
        _clKernPadBP.setArg(1, _nmax);
        _clKernPadBP.setArg(2, _bpmeshcells);
        _clKernPadBP.setArg(3, _phasespace->projectionX_buf);
    } else
    #endif // INOVESA_USE_CLFFT
    {
        _bp_padded_fft = fft_alloc_real(_nmax);
        _bp_padded = reinterpret_cast<meshdata_t*>(_bp_padded_fft);
        std::fill_n(_bp_padded,_nmax,integral_t(0));
        _formfactor_fft = fft_alloc_complex(_nmax);
        _formfactor = reinterpret_cast<impedance_t*>(_formfactor_fft);
        std::fill_n(_formfactor,_nmax,integral_t(0));
        _ffttw_bunchprofile = prepareFFT(_nmax,_bp_padded,_formfactor);
    }
}

vfps::ElectricField::ElectricField(vfps::PhaseSpace *ps,
                                   const vfps::Impedance *impedance,
                                   const double Ib, const double E0,
                                   const double sigmaE, const double dt) :
    ElectricField(ps,impedance,
                  Ib*dt*physcons::c/ps->getScale(0)/(ps->getDelta(1)*sigmaE*E0))
{
    _wakepotential = new meshaxis_t[_bpmeshcells];
    #ifdef INOVESA_USE_CL
    if (OCLH::active) {
        _wakepotential_buf = cl::Buffer(OCLH::context, CL_MEM_READ_WRITE,
                                        sizeof(*_wakepotential)*_bpmeshcells);
        #endif // INOVESA_USE_CL
        #ifndef INOVESA_USE_CLFFT
    }
        #else
        _wakelosses = new impedance_t[_nmax];
        _wakelosses_buf = cl::Buffer(OCLH::context, CL_MEM_READ_WRITE,
                                     sizeof(impedance_t)*_nmax);
        _wakepotential_complex = new impedance_t[_nmax];
        _wakepotential_complex_buf = cl::Buffer(OCLH::context,CL_MEM_READ_WRITE,
                                        sizeof(*_wakepotential_complex)*_nmax);
        const size_t nmax = _nmax;
        clfftCreateDefaultPlan(&_clfft_wakelosses,
                               OCLH::context(),CLFFT_1D,&nmax);
        clfftSetPlanPrecision(_clfft_wakelosses,CLFFT_SINGLE);
        clfftSetLayout(_clfft_wakelosses,CLFFT_COMPLEX_INTERLEAVED,
                       CLFFT_COMPLEX_INTERLEAVED);
        clfftSetResultLocation(_clfft_wakelosses, CLFFT_OUTOFPLACE);
        clfftBakePlan(_clfft_wakelosses,1,&OCLH::queue(), nullptr, nullptr);

        std::string cl_code_wakelosses = R"(
            __kernel void wakeloss(__global impedance_t* wakelosses,
                                   const __global impedance_t* impedance,
                                   const __global impedance_t* formfactor)
            {
                const uint n = get_global_id(0);
                wakelosses[n] = cmult(impedance[n],formfactor[n]);
            }
            )";

        _clProgWakelosses = OCLH::prepareCLProg(cl_code_wakelosses);
        _clKernWakelosses = cl::Kernel(_clProgWakelosses, "wakeloss");
        _clKernWakelosses.setArg(0, _wakelosses_buf);
        _clKernWakelosses.setArg(1, _impedance->data_buf);
        _clKernWakelosses.setArg(2, _formfactor_buf);

        std::string cl_code_wakepotential = R"(
            __kernel void scalewp(__global float* wakepot,
                                  const ulong paddedsize,
                                  const uint bpmeshcells,
                                  const float scaling,
                                  const __global impedance_t* wakepot_padded)
            {
                const uint g = get_global_id(0);
                const uint n = (g+bpmeshcells/2)%bpmeshcells;
                const uint p = (n+paddedsize-bpmeshcells/2)%paddedsize;
                wakepot[n] = scaling*wakepot_padded[p].real;
            }
            )";

        _clProgScaleWP = OCLH::prepareCLProg(cl_code_wakepotential);
        _clKernScaleWP = cl::Kernel(_clProgScaleWP, "scalewp");
        _clKernScaleWP.setArg(0, _wakepotential_buf);
        _clKernScaleWP.setArg(1, _nmax);
        _clKernScaleWP.setArg(2, _bpmeshcells);
        _clKernScaleWP.setArg(3, _wakescaling);
        _clKernScaleWP.setArg(4, _wakepotential_complex_buf);
    } else
    #endif // !INOVESA_USE_CLFFT
    {
        _wakelosses_fft = fft_alloc_complex(_nmax);
        _wakepotential_fft = fft_alloc_complex(_nmax);

        _wakelosses=reinterpret_cast<impedance_t*>(_wakelosses_fft);
        _wakepotential_complex=reinterpret_cast<impedance_t*>(_wakepotential_fft);
        _fft_wakelosses = prepareFFT(_nmax,_wakelosses,
                                     _wakepotential_complex,
                                     fft_direction::backward);
    }
}

// (unmaintained) constructor for use of wake function
vfps::ElectricField::ElectricField(PhaseSpace* ps, const Impedance* impedance,
                                   const double Ib, const double E0,
                                   const double sigmaE, const double dt,
                                   const double rbend, const double fs,
                                   const size_t nmax) :
        ElectricField(ps,impedance)
{
    _wakefunction = new meshaxis_t[2*_bpmeshcells];
    fftw_complex* z_fftw = fftw_alloc_complex(nmax);
    fftw_complex* zcsrf_fftw = fftw_alloc_complex(nmax);
    fftw_complex* zcsrb_fftw = fftw_alloc_complex(nmax); //for wake
    impedance_t* z = reinterpret_cast<impedance_t*>(z_fftw);
    impedance_t* zcsrf = reinterpret_cast<impedance_t*>(zcsrf_fftw);
    impedance_t* zcsrb = reinterpret_cast<impedance_t*>(zcsrb_fftw);

     const double g = - Ib*physcons::c*ps->getDelta(1)*dt
                    / (2*M_PI*fs*sigmaE*E0)/(M_PI*rbend);


    std::copy_n(_impedance->data(),std::min(nmax,_impedance->nFreqs()),z);
    if (_impedance->nFreqs() < nmax) {
        std::stringstream wavenumbers;
        wavenumbers << "(Known: n=" <<_impedance->nFreqs()
                    << ", needed: N=" << nmax << ")";
        Display::printText("Warning: Unknown impedance for high wavenumbers. "
                           +wavenumbers.str());
        std::fill_n(&z[_impedance->nFreqs()],nmax-_impedance->nFreqs(),
                    impedance_t(0));
    }

    fft_plan p3 = prepareFFT( nmax, z, zcsrf, fft_direction::forward );
    fft_plan p4 = prepareFFT( nmax, z, zcsrb, fft_direction::backward);

    fft_execute(p3);
    fft_destroy_plan(p3);
    fft_execute(p4);
    fft_destroy_plan(p4);

    /* This method works like a DFT of Z with Z(-n) = Z*(n).
     *
     * the element _wakefunction[_bpmeshcells] represents the self interaction
     * set this element (q==0) to zero to make the function anti-semetric
     */
    _wakefunction[0] = 0;
    for (size_t i=0; i< _bpmeshcells; i++) {
        // zcsrf[0].real() == zcsrb[0].real(), see comment above
        _wakefunction[_bpmeshcells-i] = g * zcsrf[i].real();
        _wakefunction[_bpmeshcells+i] = g * zcsrb[i].real();
    }
    fftw_free(z_fftw);
    fftw_free(zcsrf_fftw);
    fftw_free(zcsrb_fftw);
}

vfps::ElectricField::~ElectricField()
{
    delete [] _csrspectrum;
    delete [] _wakefunction;
    delete [] _wakepotential;

    #ifdef INOVESA_USE_CLFFT
    if (OCLH::active) {
        delete [] _bp_padded;
        delete [] _formfactor;
        delete [] _wakepotential_complex;
        clfftDestroyPlan(&_clfft_bunchprofile);
        clfftDestroyPlan(&_clfft_wakelosses);
    } else
    #endif // INOVESA_USE_CLFFT
    {
        fft_free(_bp_padded_fft);
        fft_free(_formfactor_fft);
        if(_wakelosses_fft != nullptr) {
            fft_free(_wakelosses_fft);
        }
        if(_wakepotential_fft != nullptr) {
            fft_free(_wakepotential_fft);
        }
        fft_destroy_plan(_ffttw_bunchprofile);
        if (_fft_wakelosses != nullptr) {
            fft_destroy_plan(_fft_wakelosses);
        }
        fft_cleanup();
    }
}

vfps::csrpower_t* vfps::ElectricField::updateCSR(frequency_t cutoff)
{
    _phasespace->updateXProjection();
    #ifdef INOVESA_USE_CLFFT
    if (OCLH::active) {
        clfftEnqueueTransform(_clfft_bunchprofile,CLFFT_FORWARD,1,&OCLH::queue(),
                          0,nullptr,nullptr,
                          &_bp_padded_buf(),&_formfactor_buf(),nullptr);
        OCLH::queue.enqueueBarrierWithWaitList();

        OCLH::queue.enqueueReadBuffer(_formfactor_buf,CL_TRUE,0,
                                      _nmax*sizeof(_formfactor[0]),_formfactor);
    } else
    #elif defined INOVESA_USE_CL
    if (OCLH::active) {
        _phasespace->syncCLMem(clCopyDirection::dev2cpu);
    }
    #endif // INOVESA_USE_CLTTT
    {
        // copy bunch profile so that negative times are at maximum bins
        const vfps::projection_t* bp= _phasespace->getProjection(0);
        std::copy_n(bp,_bpmeshcells/2,_bp_padded+_nmax-_bpmeshcells/2);
        std::copy_n(bp+_bpmeshcells/2,_bpmeshcells/2,_bp_padded);
        //FFT charge density
        fft_execute(_ffttw_bunchprofile);
    }
    _csrintensity = 0;
    for (unsigned int i=0; i<_nmax; i++) {
        frequency_t highpass = 1;
        if (cutoff > 0) {
            highpass -= std::exp(-std::pow((_axis_freq.scale()*_axis_freq[i]/cutoff),2));
        }

        // norm = squared magnitude
        _csrspectrum[i] = ((*_impedance)[i]).real()*std::norm(_formfactor[i]);
        _csrintensity += highpass*_csrspectrum[i];
    }

    return _csrspectrum;
}

vfps::meshaxis_t *vfps::ElectricField::wakePotential()
{
    _phasespace->updateXProjection();
    #ifdef INOVESA_USE_CLFFT
    if (OCLH::active){
        OCLH::queue.enqueueNDRangeKernel( _clKernPadBP,cl::NullRange,
                                          cl::NDRange(_bpmeshcells));
        OCLH::queue.enqueueBarrierWithWaitList();
        clfftEnqueueTransform(_clfft_bunchprofile,CLFFT_FORWARD,1,&OCLH::queue(),
                          0,nullptr,nullptr,
                          &_bp_padded_buf(),&_formfactor_buf(),nullptr);
        OCLH::queue.enqueueBarrierWithWaitList();

        OCLH::queue.enqueueNDRangeKernel( _clKernWakelosses,cl::NullRange,
                                          cl::NDRange(_nmax));
        OCLH::queue.enqueueBarrierWithWaitList();
        clfftEnqueueTransform(_clfft_wakelosses,CLFFT_BACKWARD,1,&OCLH::queue(),
                          0,nullptr,nullptr,
                          &_wakelosses_buf(),&_wakepotential_complex_buf(),nullptr);
        OCLH::queue.enqueueBarrierWithWaitList();
        OCLH::queue.enqueueNDRangeKernel( _clKernScaleWP,cl::NullRange,
                                          cl::NDRange(_nmax));
        OCLH::queue.enqueueBarrierWithWaitList();
        #ifdef INOVESA_SYNC_CL
        syncCLMem(clCopyDirection::dev2cpu);
        #endif // INOVESA_SYNC_CL
    } else
    #elif defined INOVESA_USE_CL
    if (OCLH::active) {
        _phasespace->syncCLMem(clCopyDirection::dev2cpu);
    }
    #endif // INOVESA_USE_CL
    {
        // copy bunch profile so that negative times are at maximum bins
        const vfps::projection_t* bp= _phasespace->getProjection(0);
        std::copy_n(bp,_bpmeshcells/2,_bp_padded+_nmax-_bpmeshcells/2);
        std::copy_n(bp+_bpmeshcells/2,_bpmeshcells/2,_bp_padded);

        // Fourier transorm charge density
        // FFTW R2C only computes elements 0...n/2, and
        // sets second half of output array to 0.
        // This is because Y[n-i] = Y[i].
        // We will use this, and choose the wake losses
        // for negetive frequencies to be 0, equivalent to Z(-|f|)=0.
        fft_execute(_ffttw_bunchprofile);

        for (unsigned int i=0; i<_nmax/2; i++) {
            _wakelosses[i]= (*_impedance)[i] *_formfactor[i];
        }
        std::fill_n(_wakelosses+_nmax/2,_nmax/2,0);

        //Fourier transorm wakelosses
        fft_execute(_fft_wakelosses);

        for (size_t i=0; i<_bpmeshcells/2; i++) {
            _wakepotential[_bpmeshcells/2+i]
                = _wakepotential_complex[        i].real()*_wakescaling;
            _wakepotential[_bpmeshcells/2-1-i]
                = _wakepotential_complex[_nmax-1-i].real()*_wakescaling;
        }
        #ifdef INOVESA_USE_CL
        #ifndef INOVESA_USE_CLFFT
        if (OCLH::active) {
            OCLH::queue.enqueueWriteBuffer(_wakepotential_buf,CL_TRUE,0,
                                          sizeof(*_wakepotential)*_bpmeshcells,
                                          _wakepotential);
        }
        #endif // INOVESA_USE_CLFFT
        #endif // INOVESA_USE_CL
    }
    return _wakepotential;
}

void vfps::ElectricField::syncCLMem(clCopyDirection dir)
{
    if (OCLH::active) {
    switch (dir) {
    case clCopyDirection::cpu2dev:
        OCLH::queue.enqueueWriteBuffer(_bp_padded_buf,CL_TRUE,0,
                                       sizeof(*_bp_padded)*_nmax,_bp_padded);
        OCLH::queue.enqueueWriteBuffer(_formfactor_buf,CL_TRUE,0,
                                       sizeof(*_formfactor)*_nmax,_formfactor);
        #ifdef INOVESA_USE_CLFFT
        OCLH::queue.enqueueWriteBuffer(_wakelosses_buf,CL_TRUE,0,
                                       sizeof(*_wakelosses)*_nmax,_wakelosses);
        #endif // INOVESA_USE_CLFFT
        OCLH::queue.enqueueWriteBuffer(_wakepotential_complex_buf,CL_TRUE,0,
                                       sizeof(*_wakepotential_complex)*_nmax,
                                       _wakepotential_complex);
        OCLH::queue.enqueueWriteBuffer(_wakepotential_buf,CL_TRUE,0,
                                       sizeof(*_wakepotential)*_bpmeshcells,
                                       _wakepotential);
    case clCopyDirection::dev2cpu:
        OCLH::queue.enqueueReadBuffer(_bp_padded_buf,CL_TRUE,0,
                                      sizeof(*_bp_padded)*_nmax,_bp_padded);
        OCLH::queue.enqueueReadBuffer(_formfactor_buf,CL_TRUE,0,
                                      sizeof(*_formfactor)*_nmax,_formfactor);
        #ifdef INOVESA_USE_CLFFT
        OCLH::queue.enqueueReadBuffer(_wakelosses_buf,CL_TRUE,0,
                                      sizeof(*_wakelosses)*_nmax,_wakelosses);
        #endif // INOVESA_USE_CLFFT
        OCLH::queue.enqueueReadBuffer(_wakepotential_complex_buf,CL_TRUE,0,
                                      sizeof(*_wakepotential_complex)*_nmax,
                                      _wakepotential_complex);
        OCLH::queue.enqueueReadBuffer(_wakepotential_buf,CL_TRUE,0,
                                      sizeof(*_wakepotential)*_bpmeshcells,
                                      _wakepotential);
        break;
    }
    }
}

vfps::fft_complex* vfps::ElectricField::fft_alloc_complex(size_t n)
{
    if (std::is_same<vfps::csrpower_t,float>::value) {
        return reinterpret_cast<fft_complex*>(fftwf_alloc_complex(n));
    } else if (std::is_same<vfps::csrpower_t,double>::value) {
        return reinterpret_cast<fft_complex*>(fftw_alloc_complex(n));
    }
}

vfps::integral_t* vfps::ElectricField::fft_alloc_real(size_t n)
{
    if (std::is_same<vfps::csrpower_t,float>::value) {
        return reinterpret_cast<integral_t*>(fftwf_alloc_real(n));
    } else if (std::is_same<vfps::csrpower_t,double>::value) {
        return reinterpret_cast<integral_t*>(fftw_alloc_real(n));
    }
}

void vfps::ElectricField::fft_cleanup()
{
    if (std::is_same<vfps::csrpower_t,float>::value) {
        fftwf_cleanup();
    } else if (std::is_same<vfps::csrpower_t,double>::value) {
        fftw_cleanup();
    }
}

void vfps::ElectricField::fft_destroy_plan(vfps::fft_plan plan)
{
    if (std::is_same<vfps::csrpower_t,float>::value) {
        fftwf_destroy_plan(reinterpret_cast<fftwf_plan>(plan));
    } else if (std::is_same<vfps::csrpower_t,double>::value) {
        fftw_destroy_plan(reinterpret_cast<fftw_plan>(plan));
    }
}

void vfps::ElectricField::fft_execute(const vfps::fft_plan plan)
{
    if (std::is_same<vfps::csrpower_t,float>::value) {
        fftwf_execute(reinterpret_cast<fftwf_plan>(plan));
    } else if (std::is_same<vfps::csrpower_t,double>::value) {
        fftw_execute(reinterpret_cast<fftw_plan>(plan));
    }
}

void vfps::ElectricField::fft_free(vfps::integral_t* addr)
{
    if (std::is_same<vfps::integral_t,float>::value) {
        fftwf_free(addr);
    } else if (std::is_same<vfps::integral_t,double>::value) {
        fftw_free(addr);
    }
}

void vfps::ElectricField::fft_free(vfps::fft_complex* addr)
{
    if (std::is_same<vfps::csrpower_t,float>::value) {
        fftwf_free(addr);
    } else if (std::is_same<vfps::csrpower_t,double>::value) {
        fftw_free(addr);
    }
}

vfps::fft_plan vfps::ElectricField::prepareFFT( size_t n, csrpower_t* in,
                                            impedance_t* out)
{
    fft_plan plan = nullptr;

    std::stringstream wisdomfile;
    if (std::is_same<vfps::csrpower_t,float>::value) {
        wisdomfile << "wisdom_r2c32_" << n << ".fftw";
        // use wisdomfile, if it exists
        if (fftwf_import_wisdom_from_filename(wisdomfile.str().c_str()) != 0) {
            plan = reinterpret_cast<fft_plan>(fftwf_plan_dft_r2c_1d(n,
                                         reinterpret_cast<float*>(in),
                                         reinterpret_cast<fftwf_complex*>(out),
                                         FFTW_WISDOM_ONLY|FFTW_PATIENT));
        }
        // if there was no wisdom (no or bad file), create some
        if (plan == nullptr) {
            plan = reinterpret_cast<fft_plan>(fftwf_plan_dft_r2c_1d(n,
                                         reinterpret_cast<float*>(in),
                                         reinterpret_cast<fftwf_complex*>(out),
                                         FFTW_PATIENT));
            fftwf_export_wisdom_to_filename(wisdomfile.str().c_str());
            Display::printText("Created some wisdom at "+wisdomfile.str());
        }
    } else if (std::is_same<vfps::csrpower_t,double>::value) {
        wisdomfile << "wisdom_r2c64_" << n << ".fftw";
        // use wisdomfile, if it exists
        if (fftw_import_wisdom_from_filename(wisdomfile.str().c_str()) != 0) {
            plan = reinterpret_cast<fft_plan>(fftw_plan_dft_r2c_1d(n,
                                        reinterpret_cast<double*>(in),
                                        reinterpret_cast<fftw_complex*>(out),
                                        FFTW_WISDOM_ONLY|FFTW_PATIENT));
        }
        // if there was no wisdom (no or bad file), create some
        if (plan == nullptr) {
            plan = reinterpret_cast<fft_plan>(fftw_plan_dft_r2c_1d(n,
                                        reinterpret_cast<double*>(in),
                                        reinterpret_cast<fftw_complex*>(out),
                                        FFTW_PATIENT));
            fftw_export_wisdom_to_filename(wisdomfile.str().c_str());
            Display::printText("Created some wisdom at "+wisdomfile.str());
        }
    }
    return plan;
}

vfps::fft_plan vfps::ElectricField::prepareFFT(size_t n, vfps::impedance_t* in,
                                               vfps::impedance_t* out,
                                               fft_direction direction)
{
    fft_plan plan = nullptr;

    char dir;
    int_fast8_t sign;
    if (direction == fft_direction::backward) {
        dir = 'b';
        sign = +1;
    } else {
        dir = 'f';
        sign = -1;
    }

    std::stringstream wisdomfile;
    // find filename for wisdom
    if (std::is_same<vfps::csrpower_t,float>::value) {
        wisdomfile << "wisdom_c" << dir << "c32_" << n << ".fftw";
        // use wisdomfile, if it exists
        if (fftw_import_wisdom_from_filename(wisdomfile.str().c_str()) != 0) {
            plan = reinterpret_cast<fft_plan>(fftwf_plan_dft_1d(n,
                                            reinterpret_cast<fftwf_complex*>(in),
                                            reinterpret_cast<fftwf_complex*>(out),
                                            sign,
                                            FFTW_WISDOM_ONLY|FFTW_PATIENT));
        }
        // if there was no wisdom (no or bad file), create some
        if (plan == nullptr) {
            plan = reinterpret_cast<fft_plan>(fftwf_plan_dft_1d(n,
                                            reinterpret_cast<fftwf_complex*>(in),
                                            reinterpret_cast<fftwf_complex*>(out),
                                            sign,
                                            FFTW_PATIENT));
            fftw_export_wisdom_to_filename(wisdomfile.str().c_str());
            Display::printText("Created some wisdom at "+wisdomfile.str());
        }
    } else {
        wisdomfile << "wisdom_c" << dir << "c64_" << n << ".fftw";
        // use wisdomfile, if it exists
        if (fftw_import_wisdom_from_filename(wisdomfile.str().c_str()) != 0) {
            plan = reinterpret_cast<fft_plan>(fftw_plan_dft_1d(n,
                                            reinterpret_cast<fftw_complex*>(in),
                                            reinterpret_cast<fftw_complex*>(out),
                                            sign,
                                            FFTW_WISDOM_ONLY|FFTW_PATIENT));
        }
        // if there was no wisdom (no or bad file), create some
        if (plan == nullptr) {
            plan = reinterpret_cast<fft_plan>(fftw_plan_dft_1d(n,
                                            reinterpret_cast<fftw_complex*>(in),
                                            reinterpret_cast<fftw_complex*>(out),
                                            sign,
                                            FFTW_PATIENT));
            fftw_export_wisdom_to_filename(wisdomfile.str().c_str());
            Display::printText("Created some wisdom at "+wisdomfile.str());
        }
    }
    return plan;

}

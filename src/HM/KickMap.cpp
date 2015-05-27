/******************************************************************************
 * Inovesa - Inovesa Numerical Optimized Vlesov-Equation Solver Application   *
 * Copyright (c) 2014-2015: Patrik Schönfeldt                                 *
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

#include "HM/KickMap.hpp"

vfps::KickMap::KickMap(vfps::PhaseSpace* in, vfps::PhaseSpace* out,
					const unsigned int xsize, const unsigned int ysize,
					const InterpolationType it) :
	HeritageMap(in,out,xsize,ysize,it,it),
	_force(new meshaxis_t[xsize]())
{
}

vfps::KickMap::~KickMap()
{
	delete [] _force;
}

void vfps::KickMap::apply()
{
	meshdata_t* data_in = _in->getData();
	meshdata_t* data_out = _out->getData();

	for (meshindex_t x=0; x< _xsize; x++) {
		int offset = _force[x];
		meshindex_t absoffset;
		if (offset < 0) {
			absoffset = -offset;
			for (meshindex_t y=0; y< absoffset; y++) {
				data_out[x*_ysize+y] = 0;
			}
			for (meshindex_t y=absoffset; y< _ysize; y++) {
				data_out[x*_ysize+y] = data_in[x*_ysize+y-absoffset];
			}
		} else {
			absoffset = offset;
			for (meshindex_t y=0; y< _ysize-absoffset; y++) {
				data_out[x*_ysize+y] = data_in[x*_ysize+y+absoffset];
			}
			for (meshindex_t y=_ysize-absoffset; y< _ysize; y++) {
				data_out[x*_ysize+y] = 0;
			}
		}
	}
}

void vfps::KickMap::laser(meshaxis_t amplitude,
						  meshaxis_t pulselen,
						  meshaxis_t wavelen)
{
	amplitude = amplitude*_ysize/20.0;
	meshaxis_t sinarg = 2*M_PI/(wavelen*_xsize/10.0);
	for(meshindex_t x=0; x<_xsize; x++) {
		_force[x] +=std::exp(-std::pow(-(int(x)-int(_xsize/2)),2)/(2*pulselen*pulselen))
				*amplitude*std::sin(sinarg*x);
	}
}
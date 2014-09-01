"""Usage:   parse.py dump [-f FRAMES] FILE
            parse.py peaksearch [-b BLOCKSIZE] [-f FRAMES] FILE

Arguments:
    FILE            input file (WAV)

Options:
    -h --help       show this help message and exit
    -b BLOCKSIZE    blocksize for FFT [default: 262144]
    -f FRAMESRATE   sampling frequencies [default: 2000000]
"""

from docopt import docopt 
import wave
import binascii

import struct
import numpy as np
import math

def do_fft(data, frate):
    w = np.fft.fft(data)
    freqs = np.fft.fftfreq(len(w))

    # Fine the peak in the coefficients
    idx = np.argmax(np.abs(w)**2)
    freq = freqs[idx]
    freq_in_hertz = abs(freq * frate)
    return (freqs.min(), freqs.max(), freq_in_hertz, math.sqrt(np.sum(np.abs(w)**2))/len(w))


def read_n_frame(bin_file, n):
    data = np.fromfile(bin_file, np.int8, n)
    if len(data) < n:
	return []
    else:
        return np.array(data)


def output_analysis(bin_file, frate, block_size):

    print '"blocksize"\t"i-data dominant frequency"\t"i-data sqrt of power"\t"q-data dominant frequency"\t"q-data sqrt of power"\t "complex-data dominant frequency"\t"complex_data sqrt of power"'

    data = read_n_frame(bin_file, block_size)
 
    while (len(data) > 0):
   	data_i = data[0:][::2]
   	data_q = data[1:][::2]
	data_complex = []

	for index, item in enumerate(data_i):
	        data_complex.append(complex(data_i[index], data_q[index]))

    	fft_i = do_fft(data_i, frate)
    	fft_q = do_fft(data_q, frate)
	fft_complex = do_fft(data_complex, frate)

    	print '{n}\t{fa}\t{pa}\t{fb}\t{pb}\t{fc}\t{pc}'.format(n=len(data_i),fa=fft_i[2], pa=fft_i[3], fb=fft_q[2], pb=fft_q[3], fc=fft_complex[2], pc=fft_complex[3])

	data = read_n_frame(bin_file, block_size)


if __name__=='__main__':
	
	arguments = docopt(__doc__)

	block_size = int(arguments['-b']) if arguments ['-b'] else (16 * 16384)
	
	bin_file = open(arguments['FILE'], 'rb')
	byte = bin_file.read(1)
	frate = int(arguments['-f']) if arguments ['-f'] else 2000000
	output_analysis(bin_file, frate, block_size * 2)
	
	bin_file.close();


# vim: tabstop=8 expandtab shiftwidth=4 softtabstop=4
"""Usage:   parse.py dump [-f FRAMES] FILE
            parse.py peaksearch [-b BLOCKSIZE] [-s SKIPFRAMES] [-f FRAMES] FILE

Arguments:
    FILE            input file (WAV)

Options:
    -h --help       show this help message and exit
    -b BLOCKSIZE    blocksize for FFT [default: 1024]
    -s SKIPFRAMES   number of frames to skip between exacting FFT blocks [default: 1]
    -f FRAMES       limit search to at most this number of frames
"""

# https://github.com/docopt/docopt
from docopt import docopt

# https://docs.python.org/2/library/wave.html
import wave

# http://stackoverflow.com/questions/3694918/how-to-extract-frequency-associated-with-fft-values-in-python
# https://docs.python.org/2/library/struct.html
import struct
import numpy as np
import math


def do_fft(data,frate):
    w = np.fft.fft(data)
    freqs = np.fft.fftfreq(len(w))

    # Find the peak in the coefficients
    idx=np.argmax(np.abs(w)**2)
    freq=freqs[idx]
    freq_in_hertz=abs(freq*frate)
    return (freqs.min(),freqs.max(),freq_in_hertz,math.sqrt(np.sum(np.abs(w)**2))/len(w))

def read_n_frames(wav_file, n):
    data=wav_file.readframes(n)
    data=struct.unpack('<{n}h'.format(n=n*wav_file.getnchannels()), data)
    return np.array(data)

def output_analysis(wav_file):
    print '"sample offset"\t"blocksize"\t"left dominant frequency"\t"left sqrt of power"\t"right dominant frequency"\t"right sqrt of power"'
    
    for sample_offset in range(0,max(block_size,frames-block_size),skip_frames):
        wav_file.setpos(sample_offset)

        data = read_n_frames(wav_file, block_size)

        data_a = data[0:][::2]
	for item in data_a:
		print item
        data_b = data[1:][::2]

        fft_a = do_fft(data_a,frate)
        fft_b = do_fft(data_b,frate)
        print '{o}\t{n}\t{fa}\t{pa}\t{fb}\t{pb}'.format(n=len(data_a),o=sample_offset,fa=fft_a[2], pa=fft_a[3], fb=fft_b[2], pb=fft_b[3])
    
def output_dump(data):
    data = read_n_frames(wav_file,frames)
    data_a = data[0:][::2]
    data_b = data[1:][::2]

    print '"left value"\t"right value"'

    for n in range(len(data_a)):
        print '{l}\t{r}'.format(l=data_a[n],r=data_b[n])

if __name__=='__main__':
    arguments = docopt(__doc__)

    block_size = int(arguments['-b'])
    skip_frames = int(arguments['-s'])

    wav_file=wave.open(arguments['FILE'],'r')
    frames = int(arguments['-f']) if arguments['-f'] else wav_file.getnframes()
    channels = wav_file.getnchannels()
    frate = wav_file.getframerate()

    if channels != 2:
        print 'file must be stereo'
        exit(1)

    # TODO: check 16 bit

    if arguments['peaksearch']:
        output_analysis(wav_file)

    if arguments['dump']:
        output_dump(wav_file)

    wav_file.close()

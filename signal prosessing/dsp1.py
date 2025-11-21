import numpy as np 
import matplotlib.pyplot as plt
import librosa
import soundfile as sf
from scipy import signal
import math 
from math import exp
from math import log10

#-------1.0-----------
audio = "/home/vasiliki/Desktop/uni/6 SEMESTER/ΨΕΣ /ΨΕΣ_2/music_dsp.wav"

signal, fs = librosa.load(audio, sr = 44100, mono= True) 

# Κανονικοποίηση του σήματος
max_abs_value = np.max(np.abs(signal))
normalized_signal = signal / max_abs_value

# Παράμετροι παραθυροποίησης
N = 512
frames=  int(len(normalized_signal)) // N
PN = 90.302

# Παραθυροποίηση
windowned = np.array([normalized_signal[i * N : (i + 1) * N] for i in range(frames)])

#-------1.1-----------

#Kλιμακα Bark 
def bark(f):
    return 13 * np.arctan(0.00076 * f) + 3.5 * np.arctan((f / 7500) ** 2)

frequencies = np.linspace(0, fs / 2, N // 2 + 1)
bark_frequencies = bark(frequencies)
window = np.hanning(N)

power_spectrum = []
xws = []  # to store windowed signal

for i in range(frames):
    frame = normalized_signal[i * N : (i + 1) * N]
    windowed_frame = frame * window
    xws.append(windowed_frame)
    
    spectrum = np.fft.fft(windowed_frame)
    P_k = PN + 10 * np.log10(np.abs(spectrum[:N//2 + 1])**2 + 1e-12)
    power_spectrum.append(P_k)


# # Time domain plot of 1st windowed signal
# Ts = 1 / fs
# t = np.linspace(0, Ts * (len(xws[0]) - 1), len(xws[0]))
# plt.plot(t, xws[0], linewidth=1.2)
# plt.xlabel('Time (s)')
# plt.title('Normalized music signal, 1st window')
# plt.show()

# # Power spectrum plot of 1st window
# ks = np.linspace(0, len(power_spectrum[0])-1, len(power_spectrum[0]))
# plt.plot(ks, power_spectrum[0])
# plt.xlabel('Frequency indices')
# plt.title('Power Spectrum, 1st window')
# plt.show()

#--------1.2 --------

def detect_tone_maskers(power_spectrum, frequencies):
    ST = np.zeros_like(power_spectrum, dtype=bool)
    delta_k = []

    for k in range(len(frequencies)):
        if k < 63:
            delta_k_value = 2
        elif k < 127:
            delta_k_value = 3
        elif k < 250:
            delta_k_value = 6
        else:
            delta_k_value = 1
        delta_k.append(delta_k_value)

    for k in range(1, len(power_spectrum) - 1):
        left = max(0, k - delta_k[k])
        if (
            power_spectrum[k] > power_spectrum[k-1] and
            power_spectrum[k] > power_spectrum[k+1] and
            power_spectrum[k] > power_spectrum[left] + 7
        ):
            ST[k] = True

    return ST

# --- Calculate tonal masker power ---
def calculate_tone_mask_power(ST, power_spectrum):
    PTM = np.zeros_like(power_spectrum)
    for k in range(1, len(ST) - 1):
        if ST[k]:
            PTM[k] = 10 * np.log10(
                10**(power_spectrum[k-1]/10) +
                10**(power_spectrum[k]/10) +
                10**(power_spectrum[k+1]/10)
            )
    return PTM

ST = detect_tone_maskers(power_spectrum[0], frequencies)
PTM = calculate_tone_mask_power(ST, power_spectrum[0])

ks = np.arange(len(power_spectrum[0]))  # Frequency bin indice

# plt.figure(figsize=(12, 5))
# plt.plot(ks, power_spectrum[0], label='Power Spectrum', color='gray', linewidth=1)
# plt.stem(ks[ST], power_spectrum[0][ST],
#          linefmt='b-', markerfmt='bo', basefmt=' ', label='Tonal Maskers (ST)')

# plt.xlabel('Frequency Indices (k)')
# plt.ylabel('Power (dB)')
# plt.title('Power Spectrum with Tonal Maskers and $P_{TM}$ (1st Frame)')
# plt.legend()
# plt.grid(True)
# plt.tight_layout()
# plt.show()

# plt.figure(figsize=(12, 5))
# plt.plot(ks, power_spectrum[0], label='Power Spectrum', color='gray', linewidth=1)
# plt.stem(ks[PTM > 0], PTM[PTM > 0], linefmt='b-', markerfmt='bo', basefmt=' ', label='$P_{TM}$')
# plt.xlabel('Frequency Indices (k)')
# plt.ylabel('Power (dB)')
# plt.title('$P_{TM}$ Values with Power Spectrum (1st Frame)')
# plt.legend()
# plt.grid(True)
# plt.tight_layout()
# plt.show()

#--------1.3---------

PTMc = np.load("/home/vasiliki/Desktop/uni/6 SEMESTER/ΨΕΣ /ΨΕΣ_2/P_TMc-25.npy")
PNMc = np.load("/home/vasiliki/Desktop/uni/6 SEMESTER/ΨΕΣ /ΨΕΣ_2/P_NMc-25.npy")
PTMc = PTMc.T
PNMc = PNMc.T

ks = np.arange(len(PTMc[0]))

# # plt.figure(figsize=(10, 4))
# # plt.stem(ks, PTMc[0]) 
# # plt.xlabel('Frequency Indices (k)')
# # plt.ylabel('Power (dB)')
# # plt.title('$P_{TMc}(k)$ for 1st Window')
# # plt.grid(True)
# # plt.tight_layout()
# # plt.show()

# # plt.figure(figsize=(10, 4))
# # plt.stem(ks, PNMc[0])
# # plt.xlabel('Frequency Indices (k)')
# # plt.ylabel('Power (dB)')
# # plt.title('$P_{TMc}(k)$ for 1st Window')
# # plt.grid(True)
# # plt.tight_layout()
# # plt.show()

# # Transpose if needed (depends on how your data is organized)
# PTMc = PTMc.T  # Shape: (num_windows, num_frequencies)
# PNMc = PNMc.T

# # Frequency axis (assuming 25 barks scale)
# # For MPEG psychoacoustic model, you might want to convert to Hz or Barks
# num_freq_bins = PTMc.shape[1]
# frequencies = np.linspace(0, 20000, num_freq_bins)  # Approximate human hearing range

# # Plot Tone Maskers for first window
# plt.figure(figsize=(12, 6))
# plt.plot(frequencies, PTMc[0], 'b-', label='Tone Maskers')
# plt.plot(frequencies, PNMc[0], 'r-', label='Noise Maskers')

# # Add threshold in quiet for reference
# def threshold_in_quiet(f):
#     f_khz = f/1000
#     return (3.64 * (f_khz)**-0.8 
#             - 6.5 * np.exp(-0.6 * (f_khz - 3.3)**2) 
#             + 1e-3 * (f_khz)**4)

# Tq = threshold_in_quiet(frequencies)
# plt.plot(frequencies, Tq, 'k--', label='Threshold in Quiet')

# plt.xlabel('Frequency (Hz)')
# plt.ylabel('Power (dB SPL)')
# plt.title('Psychoacoustic Maskers (First Analysis Window)')
# plt.legend()
# plt.grid(True)
# plt.xlim(20, 20000)  # Human hearing range
# plt.xscale('log')     # Logarithmic frequency scale
# plt.tight_layout()
# plt.show()

#-------1.4----------

def ftoi(f, N=512, fs=44100):
    return round(f * N / fs)

def itof(i, N=512, fs=44100):
    return i * fs / N

def b(f): 
    return 13 * np.arctan(0.00076 * f) + 3.5 * np.arctan((f / 7500.0)**2)

def compute_TTM_TNM(PTMc, PNMc, N=256, fs=44100):
    T_TMs = []
    T_NMs = []

    # TNM (Noise Maskers)
    for P_NM in PNMc:
        T_NM = np.zeros((N, N))
        js = [j for j in range(len(P_NM)) if P_NM[j] > 0]
        next_i = 0
        for j in js:
            flag = False
            for i in range(next_i, len(P_NM)):
                db = b(itof(i, N, fs)) - b(itof(j, N, fs))
                if -3 <= db < 8:
                    if not flag:
                        next_i = i
                        flag = True
                    P = P_NM[j]
                    bj = b(itof(j, N, fs))
                    if -3 <= db < -1:
                        sf = 17 * db - 0.4 * P + 11
                    elif -1 <= db < 0:
                        sf = (0.4 * P + 6) * db
                    elif 0 <= db < 1:
                        sf = -17 * db
                    elif 1 <= db < 8:
                        sf = (0.15 * P - 17) * db - 0.15 * P
                    else:
                        continue
                    T_NM[i][j] = P - 0.175 * bj + sf - 2.025
                elif db >= 8:
                    break
        T_NMs.append(T_NM)

    for P_TM in PTMc:
        T_TM = np.zeros((N, N))
        js = [j for j in range(len(P_TM)) if P_TM[j] > 0]
        next_i = 0
        for j in js:
            flag = False
            for i in range(next_i, len(P_TM)):
                db = b(itof(i, N, fs)) - b(itof(j, N, fs))
                if -3 <= db < 8:
                    if not flag:
                        next_i = i
                        flag = True
                    P = P_TM[j]
                    bj = b(itof(j, N, fs))
                    if -3 <= db < -1:
                        sf = 17 * db - 0.4 * P + 11
                    elif -1 <= db < 0:
                        sf = (0.4 * P + 6) * db
                    elif 0 <= db < 1:
                        sf = -17 * db
                    elif 1 <= db < 8:
                        sf = (0.15 * P - 17) * db - 0.15 * P
                    else:
                        continue
                    T_TM[i][j] = P - 0.275 * bj + sf - 6.025
                elif db >= 8:
                    break
        T_TMs.append(T_TM)

    return T_TMs, T_NMs
T_TMs, T_NMs = compute_TTM_TNM(PTMc, PNMc)
#--------1.5-------
def Tq(f):
    f = np.asarray(f)  # Ensure input is numpy array
    return (3.64 * (f/1000 + 0.01)**(-0.8) 
            - 6.5 * np.exp(-0.6 * (f/1000 - 3.3)**2) 
            + 1e-3 * (f/1000)**4)

plt.plot(itof(ks), Tq(itof(ks)))
plt.xscale("log")
plt.ylabel('Sound Pressure Level, SPL (dB)')
plt.xlabel('Frequency (Hz)')
plt.grid()
plt.show()

Tgs = []
for j in range(len(T_TMs)):                         # j is the index of the window
    Tgj = []
    for i in range(len(T_TMs[j])):                  # i is the index of the rows
        sums  = 0
        for m in range(len(T_TMs[j])):              # m is the index of the columns 
            sums = sums + 10 ** (0.1*T_TMs[j][i][m]) + 10 ** (0.1*T_NMs[j][i][m])
        Tgi = 10*log10(10 ** (0.1*Tq(itof(i))) + sums)
        Tgj.append(Tgi)
    Tgs.append(Tgj) 

    plt.plot(b(itof(ks)), Tgs[0])
plt.xlabel('Frequency (Bark)')
plt.ylabel('Sound Pressure Level (dB)')
plt.title('Global Masking Threshold, 1st window')
plt.grid()
plt.show()

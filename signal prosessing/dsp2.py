import numpy as np 
import matplotlib.pyplot as plt
import librosa
import soundfile as sf
from scipy import signal
import math 

#3.1
wav1  = "85005006.wav" # sperm whale
y1, sr1 = librosa.load(wav1)

wav2 = "9220100Q.wav" #humpback whale
y2, sr2 = librosa.load(wav2)

GF = 0.16
sensitivity = -155 
sensitivity_factor = pow(10, (-155/20))

pressure1 = (1.5 * y1 * GF)/sensitivity_factor
pressure2 = (1.5 * y2 * GF)/sensitivity_factor

# time1 = np.linspace(0, len(pressure1) / sr1, len(pressure1))
# time2 = np.linspace(0, len(pressure2) / sr2, len(pressure2))

# fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 6)) 

# # sperm whale
# ax1.plot(time1, pressure1)
# ax1.set_ylabel('Pressure')
# ax1.set_xlabel('Time')
# ax1.set_title('Διαγραμμα Πιεσης-Χρονου για Sperm Whale')

# # humpback whale
# ax2.plot(time2, pressure2)
# ax2.set_ylabel('Pressure')
# ax2.set_xlabel('Time')
# ax2.set_title('Διαγραμμα Πιεσης-Χρονου για Humpback Whale')

# plt.tight_layout()  
# plt.show()

#3.2
pref = 1

N1 = len(wav1)
N2 = len(wav2)

# function to calculate rms
def calculate_rms(pressure, N):
    sum_squared = 0
    for i in range(N):
        sum_squared += pressure[i]**2
    return math.sqrt(sum_squared / N)

#rms calculation
prms1 = calculate_rms(pressure1, N1)
prms2 = calculate_rms(pressure2, N2)

print(f"RMS για pressure1: {prms1}")
print(f"RMS για pressure2: {prms2}")

#SPLrms calculation 
SPL1 = 20*math.log10(prms1/pref)
SPL2 = 20*math.log10(prms2/pref)

print(f"SPLrms για pressure1: {SPL1}")
print(f"SPLrms για pressure2: {SPL2}")

#3.3 - kanoyme thn analush gia to wav1 

# sto thelorima parseval h energei atoy shmatos diatiritai
# kata th diarkia metavaseis apo to paidio toy xronoy sths suxnothtas 

# Υπολογισμός του Fourier Transform του σήματος (στον τομέα της συχνότητας)
signal_fft = np.fft.fft(pressure1)

# Ενέργεια στο πεδίο του χρόνου
energy_time = np.sum(np.abs(pressure1)**2)

# Ενέργεια στο πεδίο της συχνότητας (κανονικοποιημένη με N)
N = len(pressure1)
energy_freq = (1/N) * np.sum(np.abs(signal_fft)**2)

# Εμφάνιση των αποτελεσμάτων
print(f"Ενέργεια στο πεδίο του χρόνου: {energy_time}")
print(f"Ενέργεια στο πεδίο της συχνότητας: {energy_freq}")

# Επαλήθευση του Θεωρήματος του Parseval
if np.isclose(energy_time, energy_freq):
    print("Το θεώρημα του Parseval επιβεβαιώνεται")
else:
    print("Το θεώρημα του Parseval ΔΕΝ επιβεβαιώνεται")

# #3.4
# #koino diagramma pieshs - xronou biosimatwn 

# time1 = np.linspace(0, len(pressure1) / sr1, len(pressure1))
# time2 = np.linspace(0, len(pressure2) / sr2, len(pressure2))


# #sxediasmos Butterworth
# N = 3
# cutoff = 200
# fs1= sr1 #sampling freq
# fs2 = sr2 #sampling freq 2
# nyquist1 = fs1 / 2 
# nyquist2 = fs2 / 2
# #kanonikopoihsh cutoff 
# normal_cutoff1 = cutoff / nyquist1
# normal_cutoff2 = cutoff / nyquist2
# b1, a1 = signal.butter(N, normal_cutoff1, 'high', analog=False)
# b2, a2 = signal.butter(N, normal_cutoff2, btype='high', analog=False)
# # efarmogh butterworth filter 
# filtered_pressure1 = signal.filtfilt(b1, a1, pressure1)
# filtered_pressure2 = signal.filtfilt(b2, a2, pressure2)

# #koino diagramma pieshs-xronou meta thn efarmogh tou butterworth filtroy 
# fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 6)) 

# # sperm whale
# # before

# ax1.plot(time1, pressure1)
# ax1.set_ylabel('Pressure')
# ax1.set_xlabel('Time')
# ax1.set_title('Διαγραμμα Πιεσης-Χρονου για Sperm Whale') 
# # after
# ax2.plot(time1, filtered_pressure1)
# ax2.set_ylabel('Pressure')
# ax2.set_xlabel('Time')
# ax2.set_title('Διαγραμμα Πιεσης-Χρονου για Sperm Whale Μετα την Εφαρμογη του Φιλτρου')

# plt.tight_layout()  
# plt.show()

# # humpback whale
# fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 6)) 
# # before
# ax1.plot(time2, pressure2)
# ax1.set_ylabel('Pressure')
# ax1.set_xlabel('Time')
# ax1.set_title('Διαγραμμα Πιεσης-Χρονου για Humpback Whale')
# # after
# ax2.plot(time2, filtered_pressure2)
# ax2.set_ylabel('Pressure')
# ax2.set_xlabel('Time')
# ax2.set_title('Διαγραμμα Πιεσης-Χρονου για Humpback Whale Μετα την Εφαρμογη του Φιλτρου')

# plt.tight_layout()  
# plt.show()

#3.5

import scipy.io.wavfile as wav

#  1. Φόρτωση του αρχείου ήχου
filename = "Pile driving.wav"
fs, audio = wav.read(filename)

# Μετατροπή σε mono αν είναι στερεοφωνικό
if len(audio.shape) > 1:
    audio = np.mean(audio, axis=1)

# 2. Υπολογισμός φάσματος (FFT)
N = len(audio)
frequencies = np.fft.rfftfreq(N, 1/fs)
fft_magnitude = np.abs(np.fft.rfft(audio))

#  3. Φιλτράρισμα στις συχνότητες 300Hz - 3kHz για μεγάπτερες φάλαινες
flower, fhigher = 300, 3000
nyquist = fs / 2
low = flower / nyquist
high = fhigher / nyquist
b, a = signal.butter(4, [low, high], btype="band")

filtered_audio = signal.filtfilt(b, a, audio)

#  4. Υπολογισμός Sound Pressure Level (SPL)
# Χαρακτηριστικά υδροφώνου
GF = 0.16  # Gain Factor
Sensitivity = -125  # σε dB re 1µPa

# Υπολογισμός RMS πίεσης
Prms = np.sqrt(np.mean(filtered_audio**2))

# Μετατροπή σε dB re 1µPa
SPL_rms = 20 * np.log10(Prms / GF) + Sensitivity

# 🚀 5. Σύγκριση με το όριο των 100 dB
print(f"Υπολογισμένο SPL (RMS): {SPL_rms:.2f} dB re 1µPa")
if SPL_rms > 100:
    print(" Ο ήχος μπορεί να επηρεάσει τη συμπεριφορά των μεγάπτερων φαλαινών!")
else:
    print(" Ο ήχος δεν ξεπερνά το όριο των 100 dB και πιθανώς δεν επηρεάζει τις φάλαινες.")

# 🚀 6. Σχεδίαση γραφημάτων
plt.figure(figsize=(12, 6))

# 🎵 Αρχικό ηχητικό σήμα
plt.subplot(2, 1, 1)
plt.plot(np.linspace(0, N/fs, N), audio, alpha=0.5, label="Αρχικό σήμα")
plt.xlabel("Χρόνος [s]")
plt.ylabel("Πίεση")
plt.title("Αρχικό ηχητικό σήμα")
plt.legend()

# 🎵 Φιλτραρισμένο ηχητικό σήμα
plt.subplot(2, 1, 2)
plt.plot(np.linspace(0, N/fs, N), filtered_audio, color="red", label="Φιλτραρισμένο σήμα (300Hz - 3kHz)")
plt.xlabel("Χρόνος [s]")
plt.ylabel("Πίεση")
plt.title("Φιλτραρισμένο ηχητικό σήμα")
plt.legend()

plt.tight_layout()
plt.show()

# 🚀 7. Σχεδίαση FFT για να δούμε την ενέργεια στις σχετικές συχνότητες
plt.figure(figsize=(10, 5))
plt.plot(frequencies, 20 * np.log10(fft_magnitude), label="FFT του αρχικού ήχου", alpha=0.6)
plt.axvline(flower, color="green", linestyle="--", label="300 Hz")
plt.axvline(fhigher, color="red", linestyle="--", label="3 kHz")
plt.xlabel("Συχνότητα [Hz]")
plt.ylabel("Ενέργεια [dB]")
plt.title("Φάσμα συχνοτήτων")
plt.legend()
plt.grid()
plt.show()

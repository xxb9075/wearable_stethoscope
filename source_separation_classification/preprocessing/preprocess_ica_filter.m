%% Source separation and filtering: ICA + band filtering -> S_low and S_high
% Normalizes and time-aligns a four-channel microphone recording, separates
% it into independent components (ICs) with AMICA, and splits every component
% into a low band (cardiac) and a high band (respiratory):
%   S_low  : discrete wavelet (MODWT) band-pass with denoising, 0.5-100 Hz
%   S_high : FFT band-pass with raised-cosine edges, 100-1000 Hz
% The ICs are ranked by respiratory-band SNR (Welch PSD, 200-600 Hz against
% the rest of 0-1000 Hz); the highest-SNR IC is the respiratory component.
% The high band of every IC is exported as CSV and WAV, and the respiratory
% component additionally as <recording>_respiratory.csv/.wav.
%
% Requires EEGLAB with the AMICA plugin (runamica15) on the MATLAB path,
% the Signal Processing Toolbox and the Wavelet Toolbox.
% importfile.m (same folder) reads the *_MIC_post.csv files written by the
% host software (time column + four microphone channels).

%% Input and output
dataFilepath = fullfile('data', 'raw', 'S01', 'supine', 'recording_MIC_post.csv');  % input recording
exportFolder = fullfile('data', 'preprocessed', 'S01', 'supine');                     % output folder

[~, name, ext] = fileparts(dataFilepath);
filename = [name, ext];

%% Load the data
T171443MICpost = importfile(dataFilepath);

fs=4000;

data = table2array(T171443MICpost);
data(:,1) = [];  % drop the time column
data_norm = rescale(data - mean(data, 1), -1, 1); % normalization [-1,1], mean 0

disp("Data successfully loaded.");

%% Compensate the 40 us ADC delay between channels by linear interpolation
sampling_rate = fs;
adc_delay = 40e-6;    % 40 us ADC delay between consecutive channels
% Normalized data (zero mean, [-1, 1]) are aligned and passed to AMICA.
[aligned_data, t_ref] = align4ChannelData(data_norm, sampling_rate, adc_delay);
% Earlier version, not used: alignment of the raw ADC values
% [aligned_data, t_ref] = align4ChannelData(data, sampling_rate, adc_delay);
aligned_data(isnan(aligned_data)) = 0;
new_sampling_rate = 1/adc_delay;

% Visualize alignment
figure;
% Plot original data
time1 = (0:size(data, 1)-1) / sampling_rate; % Time vector for raw data
time2 = (0:size(aligned_data, 1)-1) / new_sampling_rate; % Time vector for aligned data

subplot(2, 1, 1);
plot(time1, data_norm(:, 1), 'k', ...
     time1, data_norm(:, 2), 'r', ...
     time1, data_norm(:, 3), 'g', ...
     time1, data_norm(:, 4), 'b'); % Plot with matching time and data dimensions
title('Original Data (With ADC Delays)');
legend('Channel 1', 'Channel 2', 'Channel 3', 'Channel 4');
xlabel('Time (s)');
ylabel('Amplitude');
grid on;

% Plot aligned data
subplot(2, 1, 2);
plot(time1, aligned_data(:, 1), 'k', ...
     time1, aligned_data(:, 2), 'r', ...
     time1, aligned_data(:, 3), 'g', ...
     time1, aligned_data(:, 4), 'b'); % Plot with matching time and aligned_data dimensions
title('Aligned Data (After Interpolation)');
legend('Channel 1', 'Channel 2 (aligned)', 'Channel 3 (aligned)', 'Channel 4 (aligned)');
xlabel('Time (s)');
ylabel('Amplitude');
grid on;

%% ICA first, then filtering
Fs = 4000;
nKeep = size(aligned_data, 1);  % keep the whole recording
% nKeep = min(size(aligned_data,1), 30*Fs);   % or only the first 30 s

data_0_6s = aligned_data(1:nKeep, :);      % nKeep x 4
t_0_6s    = (0:nKeep-1) / Fs;              % time vector in seconds

% AMICA expects channels x samples (4 x N)
data_0_6s_chx = data_0_6s.';               % 4 x nKeep

%% AMICA: separate the respiratory, cardiac and noise sources
% 4 channels, 2 models, default values for the other options.

disp("Beginning AMICA processing.");
tStart = tic;
[weights, sphere, mods] = runamica15(data_0_6s_chx, 'num_chans', 4, 'num_models', 2);
disp("AMICA processing finished.");
tEnd = toc(tStart);
fprintf("Total time to process using AMICA: %.2f min\n", tEnd/60);

% Separated sources from the unmixing matrix: S = weights * sphere * data (4 x N)
S = weights * sphere * data_0_6s_chx;

S_plt = S';

% Plot the ICA components
figure;
nComp = size(S,1);
for i = 1:nComp
    subplot(nComp,1,i);
    plot(t_0_6s, S(i,:), 'LineWidth', 1.2);
    title(sprintf('ICA Component %d', i));
    xlabel('Time/s');
    ylabel('Amplitude');
    grid on;
end

%% Filtering: two branches, 0.5-100 Hz for heart (DWT), 100-1000 Hz for lung (FFT)
S = S - mean(S, 2);

% Define bands
lowBand  = [0.5 100];    % Hz
highBand = [100 1000];   % Hz
% Earlier bands, not used:
% lowBand  = [0   80];    % Hz
% highBand = [150 1000];   % Hz
transHz  = 10;           % small cosine ramp to reduce ringing

% Filter the sources (row-wise)
S_low  = modwt_bandpass_denoise_IC(S, Fs, lowBand, 'sym8', 12);   % 4 x N
S_high = fft_bandpass_rc(S, Fs, highBand(1), highBand(2), transHz);   % 4 x N

S_low_plt = S_low';
S_high_plt = S_high';

% Plot: 4 ICs x 2 bands (left = low, right = high)
figure('Name', sprintf('AMICA Sources Filtered | Low: %g-%g Hz, High: %g-%g Hz', ...
        lowBand(1),lowBand(2), highBand(1),highBand(2)), 'Color','w');
tiledlayout(4,2,'TileSpacing','compact','Padding','compact');
for i = 1:4
    % Low band
    nexttile;
    plot(t_0_6s, S_low(i,:), 'LineWidth', 1.1); grid on;
    ylabel(sprintf('IC %d', i));
    if i==1, title(sprintf('Low (%.0f-%.0f Hz)', lowBand(1), lowBand(2))); end
    if i==4, xlabel('Time (s)'); end

    % High band
    nexttile;
    plot(t_0_6s, S_high(i,:), 'LineWidth', 1.1); grid on;
    if i==1, title(sprintf('High (%.0f-%.0f Hz)', highBand(1), highBand(2))); end
    if i==4, xlabel('Time (s)'); end
end

%% Rank the ICs by respiratory-band SNR (Welch PSD)
% SNR_dB = 10*log10(P_band / P_nonband), with P_band the PSD integrated over
% 200-600 Hz and P_nonband over the rest of 0-1000 Hz. The highest-SNR IC is
% the respiratory component, the lowest-SNR IC the noise-like component.
snrBand   = [200 600];   % Hz
snrRange  = [0 1000];    % Hz
P_band    = zeros(nComp, 1);
P_nonband = zeros(nComp, 1);
for i = 1:nComp
    [pxx, fw] = pwelch(S(i,:), [], [], [], Fs);   % default Welch settings
    P_band(i)    = bandpower(pxx, fw, snrBand, 'psd');
    P_nonband(i) = bandpower(pxx, fw, snrRange, 'psd') - P_band(i);
end
snr_dB = 10*log10(P_band ./ P_nonband);
[~, respIC]  = max(snr_dB);
[~, noiseIC] = min(snr_dB);

disp(table((1:nComp)', P_band, P_nonband, P_band./P_nonband, snr_dB, ...
    'VariableNames', {'IC', 'P_band', 'P_nonband', 'Ratio', 'SNR_dB'}));
fprintf("Respiratory IC: %d (%.2f dB), noise-like IC: %d (%.2f dB)\n", ...
    respIC, snr_dB(respIC), noiseIC, snr_dB(noiseIC));

%% Export the ICA decomposition plot
wavExportPath = fullfile(exportFolder, strcat(filename(1:end - 3), 'wav'));
if ~isfolder(exportFolder)
    mkdir(exportFolder);
end

f = gcf;  % export the filtered components plot as png
set(gcf, 'Renderer', 'painters');
exportgraphics(f, fullfile(exportFolder, strcat(filename(1:end - 4), 'ic_plots.png')));

%% Export the respiratory (high band) components as .csv and .wav
exportPath = fullfile(exportFolder, filename);
exportPath = exportPath(1:end - 4);

for channel = 1:4
    csvExportPath = strcat(exportPath, '_ic', int2str(channel), '.csv');
    wavExportPath = strcat(exportPath, '_ic', int2str(channel), '.wav');

    writematrix(S_high(channel, :), csvExportPath);  % high band of this IC
    fprintf('Data saved as .csv to %s successfully.\n', csvExportPath);

    audiowrite(wavExportPath, S_high(channel, :)/max(abs(S_high(channel, :))), fs);  % normalized for .wav
    fprintf('Data saved as .wav to %s successfully.\n', wavExportPath);
end

% Respiratory component (highest SNR) for compression_denoising.m
writematrix(S_high(respIC, :), strcat(exportPath, '_respiratory.csv'));
audiowrite(strcat(exportPath, '_respiratory.wav'), S_high(respIC, :)/max(abs(S_high(respIC, :))), fs);
fprintf('Respiratory component (IC %d) saved to %s_respiratory.csv\n', respIC, exportPath);


%% Helper functions

function Y = fft_bandpass_rc(S, Fs, f_lo, f_hi, trans)
% FFT band-pass with raised-cosine edges (two-sided mask).
% S: [C x N] real signals (rows = ICs), Fs in Hz.
% Passes |f| in [f_lo, f_hi]; trans is the width of the cosine ramps in Hz.

    if nargin < 5, trans = 0; end
    [C,N] = size(S);
    Y = zeros(C,N);

    % Two-sided frequency vector centered at 0
    if mod(N,2)==0, k = -N/2:N/2-1; else, k = -(N-1)/2:(N-1)/2; end
    f = k * (Fs/N);                 % Hz, length N
    af = abs(f);

    % Clip to physical limits
    f_lo = max(0, f_lo);
    f_hi = min(Fs/2, f_hi);
    if f_lo >= f_hi
        return;                     % empty band -> zeros
    end

    pass = zeros(1, N);

    % Lower edge
    if f_lo <= 0
        % Flat from 0 up to f_hi
        hi1 = f_hi;
        hi2 = min(Fs/2, f_hi + max(trans,1e-6));
        pass(af <= hi1) = 1;
        if hi2 > hi1
            idx = (af > hi1) & (af <= hi2);
            pass(idx) = 0.5 * (1 + cos(pi * (af(idx) - hi1) / (hi2 - hi1))); % ramp down
        end
    else
        lo1 = max(0, f_lo - trans); lo2 = f_lo;
        if lo2 > lo1
            idx = (af >= lo1) & (af < lo2);
            pass(idx) = 0.5 * (1 - cos(pi * (af(idx) - lo1) / (lo2 - lo1))); % ramp up
        end
        % Flat middle
        pass((af >= lo2) & (af <= f_hi)) = 1;
        % Right ramp
        hi2 = min(Fs/2, f_hi + max(trans,1e-6));
        if hi2 > f_hi
            idx = (af > f_hi) & (af <= hi2);
            pass(idx) = 0.5 * (1 + cos(pi * (af(idx) - f_hi) / (hi2 - f_hi))); % ramp down
        end
    end

    % Apply the mask to every IC
    X  = fft(S, [], 2);          % along time
    Xs = fftshift(X, 2);         % center zero frequency
    Xs = Xs .* pass;             % same mask for all rows
    Xf = ifftshift(Xs, 2);
    Y  = ifft(Xf, [], 2, 'symmetric');
end


function Y = modwt_bandpass_denoise_IC(S, Fs, band, wname, J)
% MODWT band-pass with wavelet denoising.
% S: [C x N] (rows = ICs), Fs in Hz.
% band = [f1 f2] Hz: keep the MODWT detail levels that overlap this band and
% soft-threshold each of them; the scaling level is kept when the band
% reaches down to near 0 Hz.
    if nargin < 4 || isempty(wname), wname = 'sym8'; end
    if nargin < 5 || isempty(J),     J     = 12;     end

    [C,N] = size(S);
    Y = zeros(C,N);

    f1 = max(0, band(1));
    f2 = min(Fs/2 - eps, band(2));
    if f1 >= f2, return; end

    % Dyadic band of detail level j: [Fs/2^(j+1), Fs/2^j]
    f_lo  = Fs ./ (2.^((1:J)+1));
    f_hi  = Fs ./ (2.^(1:J));

    for c = 1:C
        x = S(c,:);

        % MODWT (rows 1..J = details, row J+1 = scaling)
        W = modwt(x, wname, J);

        % Levels that overlap the requested band
        keep = false(1,J);
        for j = 1:J
            if (f_lo(j) <= f2) && (f_hi(j) >= f1)
                keep(j) = true;
            end
        end

        % Soft-threshold the kept detail levels
        Wsel = zeros(size(W));
        for j = 1:J
            if keep(j)
                sigma = median(abs(W(j,:))) / 0.6745 + eps;   % robust noise estimate
                thr   = sigma * sqrt(2*log(N));               % universal threshold
                Wsel(j,:) = wthresh(W(j,:), 's', thr);        % soft threshold
            end
        end

        % Keep the scaling level if the band reaches near 0 Hz
        f_scal_hi = Fs / (2^(J+1));  % scaling ~ [0, f_scal_hi]
        if f1 <= f_scal_hi
            Wsel(J+1,:) = W(J+1,:);
        end

        % Reconstruct the band-limited, denoised signal
        Y(c,:) = imodwt(Wsel, wname);
    end
end


function [data_aligned, t_ref] = align4ChannelData(data_in, fs, channel_offset)
%ALIGN4CHANNELDATA Align 4-channel microphone data by removing inter-channel delays.
%
%   [data_aligned, t_ref] = ALIGN4CHANNELDATA(data_in, fs, channel_offset)
%
%   INPUTS:
%       data_in       : n-by-4 matrix, where each column is one microphone's samples
%       fs            : Sampling rate (Hz), scalar
%       channel_offset: Time delay between consecutive channels, in seconds (e.g., 40e-6)
%
%   OUTPUTS:
%       data_aligned  : n-by-4 matrix of time-aligned signals (all channels share the same time base)
%       t_ref         : n-by-1 vector of the reference time points (seconds)
%
%   The channels are sampled one after another, so channel k lags channel 1
%   by (k-1)*channel_offset. Channels 2-4 are linearly interpolated onto the
%   time base of channel 1.

    % Validate inputs
    if size(data_in,2) ~= 4
        error('Input data must be an n-by-4 matrix (4 channels).');
    end

    if nargin < 3
        % Default offset = 40 microseconds
        channel_offset = 40e-6;
    end

    % Number of samples
    N = size(data_in, 1);

    % Time step
    dt = 1/fs;            % e.g., 1/4000 = 0.00025 s = 250 us

    % Reference time vector (channel 1)
    t_ref = (0:N-1)' * dt;  % column vector

    % Separate columns (4 channels)
    ch1 = data_in(:,1);
    ch2 = data_in(:,2);
    ch3 = data_in(:,3);
    ch4 = data_in(:,4);

    % Sample times of each channel
    t1 = t_ref;                          % channel 1: no offset
    t2 = t_ref + 1 * channel_offset;     % channel 2: 1 x offset
    t3 = t_ref + 2 * channel_offset;     % channel 3: 2 x offset
    t4 = t_ref + 3 * channel_offset;     % channel 4: 3 x offset

    % Fine time vector with 40 us steps covering channel 1 plus the largest delay
    t_new = (0:40e-6:(t_ref(end) + 3 * channel_offset))';

    % Interpolate all channels onto t_ref (linear)
    ch1_aligned = interp1(t1, ch1, t_ref, 'linear', 'extrap');
    ch2_aligned = interp1(t2, ch2, t_ref, 'linear', 'extrap');
    ch3_aligned = interp1(t3, ch3, t_ref, 'linear', 'extrap');
    ch4_aligned = interp1(t4, ch4,  t_ref, 'linear', 'extrap');
    % Earlier version, not used: spline interpolation
    % ch1_aligned = interp1(t1, ch1, t_ref, 'spline', 'extrap');
    % ch2_aligned = interp1(t2, ch2, t_ref, 'spline', 'extrap');
    % ch3_aligned = interp1(t3, ch3, t_ref, 'spline', 'extrap');
    % ch4_aligned = interp1(t4, ch4,  t_ref, 'spline', 'extrap');

    % Combine columns into one aligned dataset
    data_aligned = [ch1_aligned, ch2_aligned, ch3_aligned, ch4_aligned];

end

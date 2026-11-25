%% Automatic breath segmentation
% Finds pauses between breaths in a respiratory signal and plots the signal,
% the envelope and the detected pause regions:
%   1. zero-phase FIR band-pass, 100-1000 Hz
%   2. Hilbert amplitude envelope, smoothed with a moving average and
%      normalized to [0, 1]
%   3. adaptive threshold: a percentile of the envelope amplitude of the
%      recording; envelope minima below it are grown into pause regions
%   4. active segments shorter than the minimum breath length between two
%      pauses are merged into the pause
%
% Input: a single-column CSV with the respiratory component at 4 kHz (output
% of compression_denoising.m). Run from the source_separation_classification folder.

close all; clear; clc;

fs = 4000;
dataPath = fullfile('data', 'fully_prepared', 'S01', 'supine', 'recording_MIC_post.csv');

data = readmatrix(dataPath);    % 1-D respiratory signal

thresh = 0.08;
detection_start = 2 * fs;
detection_end = 37 * fs;

data = data(detection_start:detection_end);  % analyse 2-37 s

fprintf("Loaded data from %s.\n", dataPath);


[isPause, minInds, envelope, pauseThresh] = breath_inex_threshold_islands_plot_vseg_win(data, fs);
x_plt = 1:length(data);

figure(); hold on;
plot(data/max(data), '-', 'Color', [0.2, 0.2, 0.2, 0.1])
plot(envelope, '-b', 'LineWidth', 1.5); hold on;
scatter(x_plt(isPause), envelope(isPause), 6, 'red', 'filled', 'o');
% scatter(minInds, envelope(minInds), 20, 'cyan', 'filled', 'o');
yline(pauseThresh,'--', 'Threshold', 'LineWidth', 1);
% yline(0.04,'--', 'Threshold', 'LineWidth', 1);  % earlier fixed threshold
axis([0, length(data), -1, 1]);
legend('Normalized signal', 'Smoothed + filtered envelope', 'Identified pauses');


function [isPause, minInds, envelope, pauseThresh] = breath_inex_threshold_islands_plot_vseg_win(x, fs)
    % Envelope-based pause detection.
    %   isPause     : logical vector, true for samples of x in a pause
    %   minInds     : envelope minima below the pause threshold
    %   envelope    : smoothed Hilbert envelope, normalized to [0, 1]
    %   pauseThresh : adaptive threshold (percentile of the envelope)
    %
    % Usage:
    %   [isPause, minInds, envelope, pauseThresh] = breath_inex_threshold_islands_plot_vseg_win(x, fs);

    % -------- Parameters --------
    P.bandpass_Hz   = [100 1000]; % band-pass before the Hilbert transform (Hz)
    P.smooth_ms     = 110;        % moving-average smoothing of the envelope (ms)
    P.thresh_prctile = 15;        % adaptive threshold: percentile of the envelope
    P.min_breath_s  = 0.6;        % minimum breath length; shorter active segments become pause
    % Earlier parameters, not used:
    % P.bandpass_Hz   = [65 1600];  % (no band-pass was applied)
    % P.env_win_ms    = 60;         % RMS window length (ms)
    % P.min_seg_ms    = 120;        % minimum segment length (ms)
    % --------------------------


    %% Envelope: zero-phase FIR band-pass, Hilbert amplitude, moving average
    % bandpass() with an FIR response compensates the filter delay (zero phase).
    xf = bandpass(x, P.bandpass_Hz, fs, 'ImpulseResponse', 'fir');
    env_hilb = abs(hilbert(xf));
    env_hilb = normalize_zero_one(env_hilb);       % 0..1

    Ns      = max(1, round(P.smooth_ms/1000*fs));
    envelope   = movmean(env_hilb, Ns);               % moving-average smoothing
    envelope   = movmean(envelope, 10);

    % Earlier envelope, not used: moving RMS and an extra 4 Hz low-pass
    % N_window = max(1, round(P.env_win_ms/1000*fs));
    % env_rms  = sqrt(movmedian(x.^2, N_window));
    % env_rms  = normalize_zero_one(env_rms);
    % envelope = movmean(env_rms, Ns);
    % envelope = movmean(envelope, 10);
    % [b,a]    = butter(3, 4/(fs/2));
    % envelope = filtfilt(b, a, envelope);

    envelope   = normalize_zero_one(envelope);           % back to the full 0..1 range


    %% Pauses: envelope minima below the threshold, grown to the surrounding quiet region

    pauseThresh = prctile(envelope, P.thresh_prctile);  % adaptive, per recording
    % pauseThresh = 0.04;  % earlier fixed threshold, not used
    minInds = get_min_idx(envelope, pauseThresh, 0.2*fs);
    minBreathLen = P.min_breath_s * fs;

    isPause = getPauses(envelope, 1.1, minInds, minBreathLen);


end



%% Helper functions
% ------------------------ Helper: local minima below a threshold ------------------------
function min_indices = get_min_idx(envelope, threshold, delta_samples)
    % returns the indices of the local minima of the envelope less than the specified threshold
    % delta_samples specifies the minimum separation of subsequent local minima
    min_indices = find(islocalmin(envelope, 'MinSeparation', delta_samples) & envelope < threshold);
end

% ------------------------ Helper: normalize to [0,1] ------------------------
function y = normalize_zero_one(x)
    xmin = min(x); xmax = max(x);
    rng = xmax - xmin;
    if rng <= eps
        y = zeros(size(x));
    else
        y = (x - xmin) / rng;
    end
end



function isPause = getPauses(signal, k, minInds, minBreathLen)
% Returns a logical vector that is true where the signal is in a pause.
% Arguments:
    % signal: the envelope of the signal
    % k: a factor to determine relative pause threshold based upon value of the envelope at minInds
    % minInds: the indices of local minima below a given envelope threshold
    % minBreathLen: the minimum number of samples that a breath can be


    isPause = zeros(size(signal));
    lefts = zeros(size(minInds));
    rights = zeros(size(minInds));

    % Iterating through minima to find local area under a certain threshold
    for j = 1:length(minInds)
        local_min_ind = minInds(j);
        local_min = signal(local_min_ind);
        left = local_min_ind - 1;
        right = local_min_ind + 1;

        % Expand from the index outward until mask becomes false
        min_thresh = 0.01;
        while left > 1 && abs(signal(left)) <= max(min_thresh, k*local_min)
            left = left - 1;
        end

        while right < length(signal) && abs(signal(right)) <= max(min_thresh, k*local_min)
            right = right + 1;
        end

        lefts(j) = left;  % tracking lefts and rights
        rights(j) = right;

        isPause(left:right) = 1;
    end

    % Connect regions between isPause = true segments if close together
    for j = 1:length(lefts) - 1
        if lefts(j + 1) - rights(j) <= minBreathLen
            isPause(rights(j):lefts(j+1)) = 1;
        end
    end

    isPause = logical(isPause);

end
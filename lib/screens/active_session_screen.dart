// hide MaterialType — Flutter's material.dart exports the same name
import 'package:flutter/material.dart' hide MaterialType;
import 'dart:async';
import 'dart:math';

import '../models/recycling_session.dart'; // also exports BottleItem
import '../services/auth_service.dart';
import '../services/esp32_service.dart';
import '../services/session_service.dart';
import '../theme/app_theme.dart';
import '../widgets/responsive_layout.dart';

enum _BottleStep {
  waiting,
  detecting,
  capturing,
  classifying,
  sorting,
  bottleResult,
  sessionSummary,
}

class ActiveSessionScreen extends StatefulWidget {
  const ActiveSessionScreen({super.key});

  @override
  State<ActiveSessionScreen> createState() => _ActiveSessionScreenState();
}

class _ActiveSessionScreenState extends State<ActiveSessionScreen> {
  _BottleStep _step = _BottleStep.waiting;
  MaterialType? _currentMaterial;

  static const List<_BottleStep> _pipeline = [
    _BottleStep.detecting,
    _BottleStep.capturing,
    _BottleStep.classifying,
    _BottleStep.sorting,
  ];

  Future<void> _runBottleScan() async {
    // Step 1: Proximity sensor (~0.8s)
    setState(() => _step = _BottleStep.detecting);
    await Future.delayed(const Duration(milliseconds: 800));

    // Step 2: HuskyLens captures image (~1.0s)
    setState(() => _step = _BottleStep.capturing);
    await Future.delayed(const Duration(milliseconds: 1000));

    // Step 3: LDR/IR classifies material (~1.0s)
    setState(() => _step = _BottleStep.classifying);
    await Future.delayed(const Duration(milliseconds: 1000));
    // TODO: Replace with real material from microcontroller (Bluetooth/HTTP)
    final rng = Random();
    _currentMaterial =
        rng.nextBool() ? MaterialType.plastic : MaterialType.glass;

    // Step 4: Servo gate sorts bottle (~0.5s)
    setState(() => _step = _BottleStep.sorting);
    await Future.delayed(const Duration(milliseconds: 500));

    // Add this bottle to the active session
    final bottle = BottleItem(
      materialType: _currentMaterial!,
      earnings: RecyclingSession.earningsFor(_currentMaterial!),
      co2Saved: RecyclingSession.co2For(_currentMaterial!),
      scannedAt: DateTime.now(),
    );
    SessionService().addBottleToActive(bottle);

    setState(() => _step = _BottleStep.bottleResult);
  }

  void _addAnother() {
    setState(() {
      _step = _BottleStep.waiting;
      _currentMaterial = null;
    });
  }

  void _endSession() {
    final session = SessionService().activeSession;
    final bottleCount = session?.bottleCount ?? 0;
    SessionService().endSession();
    final uid = AuthService().currentUser?.uid ?? '';
    Esp32Service.endSession(userId: uid, bottles: bottleCount).catchError((e) {
      debugPrint('[ESP32] endSession failed: $e');
    });
    setState(() => _step = _BottleStep.sessionSummary);
  }

  void _returnHome() {
    Navigator.of(context).popUntil((route) => route.isFirst);
  }

  @override
  Widget build(BuildContext context) {
    final session = SessionService().activeSession;

    return PopScope(
      canPop: false,
      onPopInvokedWithResult: (bool didPop, _) async {
        if (didPop) return;
        final navigator = Navigator.of(context);
        final confirm = await _showCancelDialog();
        if (confirm && mounted) {
          SessionService().cancelSession();
          navigator.pop();
        }
      },
      child: ResponsiveWrapper(
        child: Scaffold(
          appBar: AppBar(
            title: Text(
              session != null
                  ? 'Session — Machine ${session.machineId}'
                  : 'Session',
            ),
            automaticallyImplyLeading:
                _step != _BottleStep.sessionSummary,
          ),
          body: SafeArea(
            child: Padding(
              padding: const EdgeInsets.all(AppSpacing.lg),
              child: switch (_step) {
                _BottleStep.waiting => _buildWaiting(session),
                _BottleStep.bottleResult => _buildBottleResult(session),
                _BottleStep.sessionSummary => _buildSessionSummary(),
                _ => _buildScanning(),
              },
            ),
          ),
        ),
      ),
    );
  }

  // ── WAITING ────────────────────────────────────────────────────────────────

  Widget _buildWaiting(RecyclingSession? session) {
    final bottles = session?.bottles ?? [];
    return Column(
      mainAxisAlignment: MainAxisAlignment.center,
      children: [
        if (bottles.isNotEmpty && session != null) _buildRunningTally(session),
        const Spacer(),
        const Icon(Icons.recycling, size: 100, color: AppColors.freshGreen),
        const SizedBox(height: 24),
        Text(
          bottles.isEmpty ? 'Place Your Bottle' : 'Place Next Bottle',
          style: const TextStyle(
            fontSize: 26,
            fontWeight: FontWeight.bold,
            color: AppColors.forestGreen,
          ),
        ),
        const SizedBox(height: 12),
        const Text(
          'Place your bottle at the machine entrance,\nthen tap Scan.',
          textAlign: TextAlign.center,
          style: AppTextStyles.bodyMedium,
        ),
        const Spacer(),
        SizedBox(
          width: double.infinity,
          height: 56,
          child: ElevatedButton.icon(
            onPressed: _runBottleScan,
            icon: const Icon(Icons.sensors, color: Colors.white),
            label: const Text('Scan Bottle'),
          ),
        ),
      ],
    );
  }

  // ── SCANNING PIPELINE ──────────────────────────────────────────────────────

  Widget _buildScanning() {
    final currentIndex = _pipeline.indexOf(_step);
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        const Text('Scanning Bottle...', style: AppTextStyles.headlineMedium),
        const SizedBox(height: 8),
        const Text(
          'Please wait — do not remove the bottle.',
          style: AppTextStyles.bodyMedium,
        ),
        const SizedBox(height: AppSpacing.xl),
        ..._pipeline.asMap().entries.map((entry) => _buildStepTile(
              step: entry.value,
              isDone: entry.key < currentIndex,
              isActive: entry.key == currentIndex,
            )),
      ],
    );
  }

  Widget _buildStepTile({
    required _BottleStep step,
    required bool isDone,
    required bool isActive,
  }) {
    final (title, subtitle, icon) = _stepInfo(step);
    final Widget leading;
    final Color textColor;

    if (isDone) {
      textColor = AppColors.textDark;
      leading = const CircleAvatar(
        radius: 20,
        backgroundColor: AppColors.freshGreen,
        child: Icon(Icons.check, color: Colors.white, size: 16),
      );
    } else if (isActive) {
      textColor = AppColors.freshGreen;
      leading = const CircleAvatar(
        radius: 20,
        backgroundColor: AppColors.forestGreen,
        child: SizedBox(
          width: 18,
          height: 18,
          child: CircularProgressIndicator(
            color: Colors.white,
            strokeWidth: 2,
          ),
        ),
      );
    } else {
      textColor = AppColors.textSubtle;
      leading = CircleAvatar(
        radius: 20,
        backgroundColor: AppColors.divider,
        child: Icon(icon, color: AppColors.textDisabled, size: 16),
      );
    }

    return Container(
      margin: const EdgeInsets.only(bottom: AppSpacing.sm),
      decoration: isActive ? AppDecorations.contentCard : null,
      child: Card(
        elevation: isActive ? 0 : 0,
        color: isActive ? AppColors.white : AppColors.scaffoldBg,
        shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(AppRadius.card)),
        child: ListTile(
          leading: leading,
          title: Text(
            title,
            style: TextStyle(
              fontWeight: isActive ? FontWeight.w600 : FontWeight.normal,
              color: textColor,
            ),
          ),
          subtitle: Text(
            isActive ? subtitle : (isDone ? 'Complete' : 'Waiting...'),
            style: TextStyle(
              color: isActive ? AppColors.freshGreen : AppColors.textSubtle,
              fontSize: 13,
            ),
          ),
        ),
      ),
    );
  }

  (String, String, IconData) _stepInfo(_BottleStep step) => switch (step) {
        _BottleStep.detecting => (
            'Proximity Sensor',
            'Detecting bottle at entrance...',
            Icons.sensors
          ),
        _BottleStep.capturing => (
            'HuskyLens Camera',
            'Capturing bottle image...',
            Icons.camera_alt
          ),
        _BottleStep.classifying => (
            'LDR / IR Sensors',
            'Classifying material...',
            Icons.science
          ),
        _BottleStep.sorting => (
            'Servo Gate',
            'Activating sorting gate...',
            Icons.settings
          ),
        _ => ('', '', Icons.circle),
      };

  // ── BOTTLE RESULT ──────────────────────────────────────────────────────────

  Widget _buildBottleResult(RecyclingSession? session) {
    final m = _currentMaterial!;
    final earnings = RecyclingSession.earningsFor(m);
    final isPlastic = m == MaterialType.plastic;
    final bottles = session?.bottles ?? [];

    return SingleChildScrollView(
      child: Column(
        children: [
          const Icon(Icons.check_circle, size: 64, color: AppColors.freshGreen),
          const SizedBox(height: 12),
          Text(
            'Bottle ${bottles.length} Scanned',
            style: const TextStyle(
              fontSize: 24,
              fontWeight: FontWeight.bold,
              color: AppColors.forestGreen,
            ),
          ),
          const SizedBox(height: AppSpacing.lg),

          // This bottle's result
          Container(
            decoration: AppDecorations.contentCard,
            padding: const EdgeInsets.all(AppSpacing.lg),
            child: Column(
              children: [
                _resultRow(
                  icon: isPlastic ? Icons.water_drop : Icons.local_drink,
                  label: 'Material',
                  value: isPlastic ? 'Plastic' : 'Glass',
                  color: isPlastic ? AppColors.freshGreen : AppColors.midGreen,
                ),
                const Divider(),
                _resultRow(
                  icon: Icons.attach_money,
                  label: 'Earnings',
                  value: 'GHS ${earnings.toStringAsFixed(2)}',
                  color: AppColors.earningsGreen,
                ),
              ],
            ),
          ),
          const SizedBox(height: AppSpacing.md),

          if (session != null) _buildRunningTally(session),
          const SizedBox(height: AppSpacing.lg),

          SizedBox(
            width: double.infinity,
            height: 56,
            child: ElevatedButton.icon(
              onPressed: _addAnother,
              icon: const Icon(Icons.add, color: Colors.white),
              label: const Text('Add Another Bottle'),
            ),
          ),
          const SizedBox(height: AppSpacing.sm),
          SizedBox(
            width: double.infinity,
            height: 56,
            child: OutlinedButton.icon(
              onPressed: _endSession,
              icon: const Icon(Icons.done_all),
              label: const Text('Done — End Session'),
            ),
          ),
        ],
      ),
    );
  }

  // ── SESSION SUMMARY ────────────────────────────────────────────────────────

  Widget _buildSessionSummary() {
    final sessions = SessionService().completedSessions;
    if (sessions.isEmpty) return const SizedBox();
    final session = sessions.first;

    return SingleChildScrollView(
      child: Column(
        children: [
          const Icon(Icons.emoji_events, size: 72, color: AppColors.forestGreen),
          const SizedBox(height: AppSpacing.md),
          const Text(
            'Session Complete!',
            style: TextStyle(
              fontSize: 26,
              fontWeight: FontWeight.bold,
              color: AppColors.forestGreen,
            ),
          ),
          const SizedBox(height: AppSpacing.sm),
          const Text(
            'Your earnings have been credited to your wallet.',
            textAlign: TextAlign.center,
            style: AppTextStyles.bodyMedium,
          ),
          const SizedBox(height: AppSpacing.lg),

          // Summary totals card
          Container(
            decoration: AppDecorations.contentCard,
            padding: const EdgeInsets.all(AppSpacing.lg),
            child: Column(
              children: [
                _resultRow(
                  icon: Icons.format_list_numbered,
                  label: 'Bottles',
                  value: '${session.bottleCount}',
                  color: AppColors.sessionGreen,
                ),
                const Divider(),
                _resultRow(
                  icon: Icons.attach_money,
                  label: 'Total Earnings',
                  value: 'GHS ${session.totalEarnings.toStringAsFixed(2)}',
                  color: AppColors.earningsGreen,
                ),
                const Divider(),
                _resultRow(
                  icon: Icons.eco,
                  label: 'CO\u2082 Saved',
                  value: '${session.totalCo2Saved.toStringAsFixed(2)} kg',
                  color: AppColors.co2DeepGreen,
                ),
              ],
            ),
          ),
          const SizedBox(height: AppSpacing.lg),

          const Align(
            alignment: Alignment.centerLeft,
            child: Text('Bottle Breakdown', style: AppTextStyles.headlineMedium),
          ),
          const SizedBox(height: AppSpacing.sm),
          ...session.bottles.asMap().entries.map((entry) {
            final i = entry.key;
            final b = entry.value;
            final isPlastic = b.materialType == MaterialType.plastic;
            return Container(
              margin: const EdgeInsets.only(bottom: AppSpacing.sm),
              decoration: AppDecorations.contentCard,
              child: ListTile(
                leading: CircleAvatar(
                  backgroundColor: AppColors.mintGreen,
                  child: Icon(
                    isPlastic ? Icons.water_drop : Icons.local_drink,
                    color: isPlastic
                        ? AppColors.freshGreen
                        : AppColors.midGreen,
                    size: 20,
                  ),
                ),
                title: Text('Bottle ${i + 1}: ${b.materialLabel}'),
                subtitle: Text('GHS ${b.earnings.toStringAsFixed(2)}'),
                trailing: Text(
                  'GHS ${b.earnings.toStringAsFixed(2)}',
                  style: const TextStyle(
                    color: AppColors.earningsGreen,
                    fontWeight: FontWeight.bold,
                  ),
                ),
              ),
            );
          }),
          const SizedBox(height: AppSpacing.lg),
          SizedBox(
            width: double.infinity,
            height: 56,
            child: ElevatedButton.icon(
              onPressed: _returnHome,
              icon: const Icon(Icons.home, color: Colors.white),
              label: const Text('Return Home'),
            ),
          ),
        ],
      ),
    );
  }

  // ── SHARED WIDGETS ─────────────────────────────────────────────────────────

  Widget _buildRunningTally(RecyclingSession session) {
    return Container(
      padding: const EdgeInsets.symmetric(
          horizontal: AppSpacing.md, vertical: AppSpacing.sm),
      decoration: BoxDecoration(
        color: AppColors.mintGreen,
        borderRadius: BorderRadius.circular(AppRadius.card),
        border: Border.all(color: AppColors.freshGreen.withValues(alpha: 0.4)),
      ),
      child: Row(
        mainAxisAlignment: MainAxisAlignment.spaceAround,
        children: [
          _tallyItem('${session.bottleCount}', 'Bottles',
              Icons.format_list_numbered),
          _tallyItem('GHS ${session.totalEarnings.toStringAsFixed(2)}',
              'Earnings', Icons.attach_money),
        ],
      ),
    );
  }

  Widget _tallyItem(String value, String label, IconData icon) {
    return Column(
      children: [
        Icon(icon, size: 20, color: AppColors.forestGreen),
        const SizedBox(height: 4),
        Text(
          value,
          style: const TextStyle(
            fontWeight: FontWeight.bold,
            fontSize: 13,
            color: AppColors.forestGreen,
          ),
        ),
        Text(label, style: AppTextStyles.labelSmall),
      ],
    );
  }

  Widget _resultRow({
    required IconData icon,
    required String label,
    required String value,
    required Color color,
  }) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 10),
      child: Row(
        children: [
          Icon(icon, color: color, size: 26),
          const SizedBox(width: 12),
          Expanded(
            child: Text(label, style: AppTextStyles.bodyMedium),
          ),
          Text(
            value,
            style: TextStyle(
              fontSize: 15,
              fontWeight: FontWeight.bold,
              color: color,
            ),
          ),
        ],
      ),
    );
  }

  Future<bool> _showCancelDialog() async {
    return await showDialog<bool>(
          context: context,
          builder: (_) => AlertDialog(
            title: const Text('Cancel Session?'),
            content: const Text(
              'Leaving now will discard this session and no earnings will be credited.',
            ),
            actions: [
              TextButton(
                onPressed: () => Navigator.pop(context, false),
                child: const Text('Stay'),
              ),
              TextButton(
                onPressed: () => Navigator.pop(context, true),
                child: const Text(
                  'Cancel Session',
                  style: TextStyle(color: Colors.red),
                ),
              ),
            ],
          ),
        ) ??
        false;
  }
}

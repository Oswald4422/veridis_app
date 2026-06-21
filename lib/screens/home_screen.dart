import 'package:flutter/material.dart';
import 'dart:async';
import 'package:app_links/app_links.dart';
import 'statistics_screen.dart';
import 'leaderboard_screen.dart';
import 'profile_screen.dart';
import 'qr_scan_screen.dart';
import 'history_screen.dart';
import 'wallet_screen.dart';
import 'active_session_screen.dart';
import '../services/auth_service.dart';
import '../services/session_service.dart';
import '../services/wallet_service.dart';
import '../services/esp32_service.dart';
import '../theme/app_theme.dart';
import '../widgets/responsive_layout.dart';

class HomeScreen extends StatefulWidget {
  const HomeScreen({super.key});

  @override
  State<HomeScreen> createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen> {
  int _selectedIndex = 0;
  String _firstName = '';
  StreamSubscription? _linkSub;
  final _appLinks = AppLinks();

  @override
  void initState() {
    super.initState();
    _loadName();
    _initDeepLinks();
  }

  Future<void> _initDeepLinks() async {
    try {
      final uri = await _appLinks.getInitialLink();
      if (uri != null && uri.toString().contains('veridis://connect')) {
        _handleDeepLink();
      }
    } catch (_) {}

    _linkSub = _appLinks.uriLinkStream.listen((uri) {
      if (uri.toString().contains('veridis://connect')) {
        _handleDeepLink();
      }
    }, onError: (_) {});
  }

  void _handleDeepLink() {
    if (!mounted) return;
    SessionService().startSession('esp32_machine');
    final uid = AuthService().currentUser?.uid ?? '';
    Esp32Service.startSession(name: _firstName, userId: uid).catchError((_) {});
    Navigator.push(
      context,
      MaterialPageRoute(builder: (_) => const ActiveSessionScreen()),
    );
  }

  @override
  void dispose() {
    _linkSub?.cancel();
    super.dispose();
  }

  Future<void> _loadName() async {
    final data = await AuthService().getUserDoc();
    if (!mounted) return;
    final full = data?['name'] as String? ?? '';
    if (full.isEmpty) {
      setState(() => _selectedIndex = 3);
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Please complete your profile details.')),
      );
    } else {
      setState(() => _firstName = full.split(' ').first);
    }
  }

  // Getter (not const) so Statistics/Profile always rebuild with fresh data
  List<Widget> get _pages => [
        const HomeContent(),
        StatisticsScreen(),
        LeaderboardScreen(),
        ProfileScreen(),
      ];

  void _onItemTapped(int index) {
    setState(() => _selectedIndex = index);
  }

  @override
  Widget build(BuildContext context) {
    return ResponsiveWrapper(
      child: Scaffold(
        appBar: AppBar(
          title: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            mainAxisSize: MainAxisSize.min,
            children: [
              const Text(
                'Good day,',
                style: AppTextStyles.heroCaption,
              ),
              Text(
                _firstName.isEmpty ? 'VERIDIS' : _firstName,
                style: const TextStyle(
                  color: Colors.white,
                  fontSize: 20,
                  fontWeight: FontWeight.w700,
                ),
              ),
            ],
          ),
          actions: [
            IconButton(
              icon: const Icon(Icons.notifications_outlined),
              onPressed: () {},
            ),
          ],
        ),
        body: _pages[_selectedIndex],
        bottomNavigationBar: BottomNavigationBar(
          items: const <BottomNavigationBarItem>[
            BottomNavigationBarItem(icon: Icon(Icons.home), label: 'Home'),
            BottomNavigationBarItem(
                icon: Icon(Icons.analytics), label: 'Statistics'),
            BottomNavigationBarItem(
                icon: Icon(Icons.leaderboard), label: 'Leaderboard'),
            BottomNavigationBarItem(icon: Icon(Icons.person), label: 'Profile'),
          ],
          currentIndex: _selectedIndex,
          onTap: _onItemTapped,
        ),
      ),
    );
  }
}

class HomeContent extends StatefulWidget {
  const HomeContent({super.key});

  @override
  State<HomeContent> createState() => _HomeContentState();
}

class _HomeContentState extends State<HomeContent> {
  final _svc = SessionService();
  final _wallet = WalletService();

  final List<String> _funFacts = [
    'Recycling one ton of plastic saves enough energy to power a home for 2–3 months.',
    'Glass can be recycled endlessly without loss of quality.',
    'Recycling reduces greenhouse gas emissions by up to 70%.',
    'Plastic takes up to 1,000 years to decompose in a landfill.',
  ];
  int _currentFactIndex = 0;
  Timer? _factTimer;

  @override
  void initState() {
    super.initState();
    _factTimer = Timer.periodic(const Duration(seconds: 12), (_) {
      setState(() {
        _currentFactIndex = (_currentFactIndex + 1) % _funFacts.length;
      });
    });
  }

  @override
  void dispose() {
    _factTimer?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return SingleChildScrollView(
      padding: const EdgeInsets.all(AppSpacing.md),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const SizedBox(height: AppSpacing.sm),

          // ── Hero balance card ──────────────────────────────────────────
          Container(
            width: double.infinity,
            padding: const EdgeInsets.all(AppSpacing.lg),
            decoration: AppDecorations.heroCard,
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                const Text('Total Earnings', style: AppTextStyles.heroLabel),
                const SizedBox(height: 6),
                Text(
                  'GHS ${_wallet.balance.toStringAsFixed(2)}',
                  style: AppTextStyles.heroDisplayLarge,
                ),
                const SizedBox(height: AppSpacing.md),
                const Divider(color: Color(0x33FFFFFF), height: 1),
                const SizedBox(height: AppSpacing.md),
                Row(
                  children: [
                    _heroStat(
                      '${_svc.totalBottleCount}',
                      'Bottles',
                    ),
                    Container(
                      width: 1,
                      height: 32,
                      color: const Color(0x33FFFFFF),
                      margin: const EdgeInsets.symmetric(
                          horizontal: AppSpacing.md),
                    ),
                    _heroStat(
                      '${_svc.sessionCount}',
                      'Sessions',
                    ),
                    Container(
                      width: 1,
                      height: 32,
                      color: const Color(0x33FFFFFF),
                      margin: const EdgeInsets.symmetric(
                          horizontal: AppSpacing.md),
                    ),
                    _heroStat(
                      '${_svc.totalCo2Saved.toStringAsFixed(1)} kg',
                      'CO\u2082 Saved',
                    ),
                  ],
                ),
              ],
            ),
          ),
          const SizedBox(height: AppSpacing.lg),

          // ── Metric cards ───────────────────────────────────────────────
          Row(
            children: [
              Expanded(
                child: _buildMetricCard(
                  icon: Icons.recycling,
                  value: '${_svc.totalBottleCount}',
                  label: 'Bottles Recycled',
                ),
              ),
              const SizedBox(width: AppSpacing.md),
              Expanded(
                child: _buildMetricCard(
                  icon: Icons.account_balance_wallet,
                  value: 'GHS ${_wallet.balance.toStringAsFixed(2)}',
                  label: 'Wallet Balance',
                  isEarnings: true,
                ),
              ),
            ],
          ),
          const SizedBox(height: AppSpacing.md),
          Row(
            children: [
              Expanded(
                child: _buildMetricCard(
                  icon: Icons.eco,
                  value: '${_svc.totalCo2Saved.toStringAsFixed(2)} kg',
                  label: 'CO\u2082 Saved',
                ),
              ),
              const SizedBox(width: AppSpacing.md),
              Expanded(
                child: _buildMetricCard(
                  icon: Icons.access_time,
                  value: '${_svc.sessionCount}',
                  label: 'Sessions',
                ),
              ),
            ],
          ),
          const SizedBox(height: AppSpacing.lg),

          // ── Fun Fact ───────────────────────────────────────────────────
          Container(
            padding: const EdgeInsets.all(AppSpacing.md),
            decoration: AppDecorations.contentCard,
            child: Row(
              children: [
                const Icon(
                  Icons.lightbulb_outline,
                  color: AppColors.freshGreen,
                  size: 28,
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: Text(
                    _funFacts[_currentFactIndex],
                    style: AppTextStyles.bodyLarge,
                  ),
                ),
              ],
            ),
          ),
          const SizedBox(height: AppSpacing.lg),

          // ── Quick Actions ──────────────────────────────────────────────
          const Text('Quick Actions', style: AppTextStyles.headlineMedium),
          const SizedBox(height: AppSpacing.md),
          // Primary action — full width
          SizedBox(
            width: double.infinity,
            height: 56,
            child: ElevatedButton.icon(
              onPressed: () {
                Navigator.push(
                  context,
                  MaterialPageRoute(builder: (_) => const QrScanScreen()),
                ).then((_) => setState(() {}));
              },
              icon: const Icon(Icons.qr_code_scanner, size: 22),
              label: const Text('Start Session'),
              style: ElevatedButton.styleFrom(
                textStyle: const TextStyle(
                  fontSize: 15,
                  fontWeight: FontWeight.w600,
                ),
              ),
            ),
          ),
          const SizedBox(height: AppSpacing.sm),
          // Secondary actions — side by side
          Row(
            children: [
              Expanded(
                child: _buildSecondaryAction(
                  icon: Icons.account_balance_wallet_outlined,
                  label: 'My Wallet',
                  onTap: () {
                    Navigator.push(
                      context,
                      MaterialPageRoute(builder: (_) => const WalletScreen()),
                    ).then((_) => setState(() {}));
                  },
                ),
              ),
              const SizedBox(width: AppSpacing.sm),
              Expanded(
                child: _buildSecondaryAction(
                  icon: Icons.history,
                  label: 'History',
                  onTap: () {
                    Navigator.push(
                      context,
                      MaterialPageRoute(builder: (_) => const HistoryScreen()),
                    );
                  },
                ),
              ),
            ],
          ),
          const SizedBox(height: AppSpacing.md),
        ],
      ),
    );
  }

  Widget _heroStat(String value, String label) {
    return Expanded(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(value, style: AppTextStyles.heroTitle),
          const SizedBox(height: 2),
          Text(label, style: AppTextStyles.heroCaption),
        ],
      ),
    );
  }

  Widget _buildMetricCard({
    required IconData icon,
    required String value,
    required String label,
    bool isEarnings = false,
  }) {
    return Container(
      padding: const EdgeInsets.all(AppSpacing.md),
      decoration: AppDecorations.contentCard,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Container(
            width: 40,
            height: 40,
            decoration: BoxDecoration(
              color: AppColors.mintGreen,
              borderRadius: BorderRadius.circular(AppRadius.chip),
            ),
            child: Icon(icon, size: 22, color: AppColors.freshGreen),
          ),
          const SizedBox(height: AppSpacing.sm),
          Text(
            value,
            style: TextStyle(
              fontSize: 16,
              fontWeight: FontWeight.w700,
              color: isEarnings
                  ? AppColors.earningsGreen
                  : AppColors.textDark,
            ),
          ),
          const SizedBox(height: 2),
          Text(label, style: AppTextStyles.bodyMedium),
        ],
      ),
    );
  }

  Widget _buildSecondaryAction({
    required IconData icon,
    required String label,
    required VoidCallback onTap,
  }) {
    return OutlinedButton.icon(
      onPressed: onTap,
      icon: Icon(icon, size: 18),
      label: Text(label),
      style: OutlinedButton.styleFrom(
        padding: const EdgeInsets.symmetric(vertical: 14),
        side: const BorderSide(color: AppColors.forestGreen),
        foregroundColor: AppColors.forestGreen,
        textStyle: const TextStyle(
          fontSize: 13,
          fontWeight: FontWeight.w600,
        ),
      ),
    );
  }
}

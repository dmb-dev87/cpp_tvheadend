/*
 * Elementary Stream Filters
 */

tvheadend.esfilter_tab = function(panel)
{
    tvheadend.idnode_grid(panel, {
        url: 'api/esfilter/video',
        titleS: 'Video Stream Filter',
        titleP: 'Video Stream Filters',
        tabIndex: 0,
        add: {
            url: 'api/esfilter/video',
            create: {}
        },
        del: true,
        move: true,
        help: function() {
            new tvheadend.help('Elementary Stream Filter', 'config_esfilter.html');
        }
    });

    tvheadend.idnode_grid(panel, {
        url: 'api/esfilter/audio',
        titleS: 'Audio Stream Filter',
        titleP: 'Audio Stream Filters',
        tabIndex: 1,
        add: {
            url: 'api/esfilter/audio',
            create: {}
        },
        del: true,
        move: true,
        help: function() {
            new tvheadend.help('Elementary Stream Filter', 'config_esfilter.html');
        }
    });

    tvheadend.idnode_grid(panel, {
        url: 'api/esfilter/teletext',
        titleS: 'Teletext Stream Filter',
        titleP: 'Teletext Stream Filters',
        tabIndex: 2,
        add: {
            url: 'api/esfilter/teletext',
            create: {}
        },
        del: true,
        move: true,
        help: function() {
            new tvheadend.help('Elementary Stream Filter', 'config_esfilter.html');
        }
    });

    tvheadend.idnode_grid(panel, {
        url: 'api/esfilter/subtit',
        titleS: 'Subtitle Stream Filter',
        titleP: 'Subtitle Stream Filters',
        tabIndex: 3,
        add: {
            url: 'api/esfilter/subtit',
            create: {}
        },
        del: true,
        move: true,
        help: function() {
            new tvheadend.help('Elementary Stream Filter', 'config_esfilter.html');
        }
    });

    tvheadend.idnode_grid(panel, {
        url: 'api/esfilter/ca',
        titleS: 'CA Stream Filter',
        titleP: 'CA Stream Filters',
        tabIndex: 4,
        add: {
            url: 'api/esfilter/ca',
            create: {}
        },
        del: true,
        move: true,
        help: function() {
            new tvheadend.help('Elementary Stream Filter', 'config_esfilter.html');
        }
    });

    tvheadend.idnode_grid(panel, {
        url: 'api/esfilter/other',
        titleS: 'Other Stream Filter',
        titleP: 'Other Stream Filters',
        tabIndex: 5,
        add: {
            url: 'api/esfilter/other',
            create: {}
        },
        del: true,
        move: true,
        help: function() {
            new tvheadend.help('Elementary Stream Filter', 'config_esfilter.html');
        }
    });
};

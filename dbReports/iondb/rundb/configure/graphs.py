# Copyright (C) 2012 Ion Torrent Systems, Inc. All Rights Reserved
'''Functions supporting graphs generated on the Services page'''
import os
import math
from django import http
os.environ['MPLCONFIGDIR'] = '/tmp'
import matplotlib
from matplotlib.backends.backend_agg import FigureCanvasAgg as FigureCanvas
from matplotlib.figure import Figure

from iondb.rundb import models
from iondb.utils.files import disk_attributes
from iondb.rundb.data import dmactions_types

IONGREEN = [0.0, 0.8, 0.3]  # green
IONYELLOW = [1.0, 0.85, 0.0]  # yellow
IONORANGE = [1.0, 0.6, 0.0]  # orange
IONRED = [0.85, 0.1, 0.1]  # red
IONBLUE = [0.0, 0.4375, .9375]  # blue


def bargraph():
    figwidth = 7
    figheight = 1.5
    matplotlib.rcParams['font.size'] = 10.0
    matplotlib.rcParams['axes.titlesize'] = 14.0
    matplotlib.rcParams['xtick.labelsize'] = 10.0
    matplotlib.rcParams['legend.fontsize'] = 10.0
    fig = Figure(figsize=(figwidth, figheight))
    fig.subplots_adjust(bottom=0.3, top=0.75, left=0.01, right=0.95)
    fig.patch.set_alpha(0)
    ax = fig.add_subplot(1, 1, 1)
    ax.set_yticks([])
    return fig, ax


def archive_graph_bar(request):
    ''' Generates a bar graph to display the percentage of runs
    that belong to each storage option.'''
    # create figure
    fig, ax = bargraph()

    runs = models.Experiment.objects.all()
    # Get only Experiments that are still on fileserver: not deleted or archived.
    runs = runs.exclude(expName__in=models.Backup.objects.all().values('backupName'))
    num_arch = runs.filter(storage_options='A').count()
    num_del = runs.filter(storage_options='D').count()
    num_keep = runs.filter(storage_options='KI').count()
    total = sum([num_arch, num_del, num_keep])
    frac_arch = 0 if num_arch == 0 else (float(num_arch) / float(total)) * 100
    frac_del = 0 if num_del == 0 else (float(num_del) / float(total)) * 100
    frac_keep = 0 if num_keep == 0 else (float(num_keep) / float(total)) * 100
    frac = [frac_arch, frac_del, frac_keep]
    if float(frac[0] + frac[1] + frac[2]) > 100.0:
        frac[2] = frac[2] - (float(frac[0] + frac[1] + frac[2]) - 100.0)

    i = 0
    colors = [IONYELLOW, IONORANGE, IONBLUE]
    for j, fr in enumerate(frac):
        ax.barh(bottom=0, width=fr, left=i, height=1, color=colors[j])
        i = i + fr

    ax.set_title('Storage Option Breakdown')
    ax.set_xlabel('% of runs')
    if frac[0] >= 15:
        ax.text(float(frac[0]) / 200,
                0.5,
                'Archive',
                transform=ax.transAxes,
                fontsize=10,
                verticalalignment='center',
                horizontalalignment='center')
    if frac[1] >= 15:
        ax.text(float(frac[1]) / 200 + float(frac[0]) / 100,
                0.5,
                'Delete',
                transform=ax.transAxes,
                fontsize=10,
                verticalalignment='center',
                horizontalalignment='center')
    if frac[2] >= 10:
        ax.text(
            float(frac[0]) / 100 + float(frac[1]) / 100 + float(frac[2]) / 200,
            0.5,
            'Keep',
            transform=ax.transAxes,
            fontsize=10,
            verticalalignment='center',
            horizontalalignment='center')

    canvas = FigureCanvas(fig)
    response = http.HttpResponse(content_type='image/png')
    canvas.print_png(response)
    return response


def fs_statusbar(request, percentFull):
    '''Creates graph of percent disk space used'''

    full = float(percentFull)
    #threshold = float(models.BackupConfig.get().backup_threshold)
    try:
        threshold = models.DMFileSet.objects.filter(type=dmactions_types.SIG).order_by('-pk')
        threshold = float(threshold[0].auto_trigger_usage)
    except:
        threshold = 0
    if threshold == 0:
        threshold = float(0.01)

    # Define colors for the used disk space area based on threshold setting
    if full <= threshold * 0.85:
        color = IONGREEN
    elif full <= threshold * 0.90:
        color = IONYELLOW
    elif full < threshold:
        color = IONORANGE
    else:
        color = IONRED

    # create figure
    fig, ax = bargraph()

    ax.set_title('File Server Space')
    ax.set_xlabel('% Capacity')

    frac = [full, 100 - full]
    i = 0
    for fr in frac:
        ax.barh(bottom=0, width=fr, height=0.2, left=i, color=color)
        i += fr
        color = IONBLUE

    # Place vertical bar indicating threshold level.  Threshold label is moved
    # up when its close to the bar graph right edge.
    if threshold >= 88:
        textHeight = 1.2
        textHoriz = 0.90
    else:
        textHeight = 0.85
        textHoriz = float(threshold) / 100 + 0.02

    ax.axvline(threshold, 0, 1, color='#000000', linewidth=3, marker='d', markersize=14)
    ax.text(textHoriz,
            textHeight,
            'Threshold',
            transform=ax.transAxes,
            fontsize=10,
            verticalalignment='center')

    # Fill in bar graph with colors representing disk capacity use
    ax.text(float(full) / 200,
            0.5,
            'Used',
            transform=ax.transAxes,
            fontsize=10,
            verticalalignment='center',
            horizontalalignment='center')

    if int(full) >= 10:
        ax.text((100 - float(full)) / 200 + float(full) / 100,
                0.5,
                'Free',
                transform=ax.transAxes,
                fontsize=10,
                verticalalignment='center',
                horizontalalignment='center')

    canvas = FigureCanvas(fig)
    response = http.HttpResponse(content_type='image/png')
    canvas.print_png(response)
    return response


def archive_drivespace_bar(request, pk):
    '''Displays as a horizontal bar chart, the free vs used space
    on the archive drive.  Will only display if it is mounted'''
    try:
        bk = models.DMFileSet.objects.get(pk=pk)
        # create figure
        fig, ax = bargraph()

        if bk.backup_directory == 'None':
            #return http.HttpResponse()
            used_frac = 0
            free_frac = 0
            title = "<Archive not Configured>"
            labels = ['', '']
        else:
            try:
                totalSpace, availSpace, f, b = disk_attributes(bk.backup_directory)
                used_frac = (float(totalSpace - availSpace) / float(totalSpace))
                free_frac = 1 - used_frac
                title = 'Archive: %s' % bk.backup_directory
                labels = ['Used', 'Free']
            except:
                used_frac = 1
                free_frac = 0
                title = 'Archive: %s' % bk.backup_directory
                labels = ["Error: Could not get drive statistics",""]

        frac = [used_frac * 100, free_frac * 100]

        if float(frac[0] + frac[1]) > 100.0:
            frac[1] = frac[1] - (float(frac[0] + frac[1]) - 100.0)

        color = IONYELLOW
        i = 0
        for fr in frac:
            ax.barh(bottom=0, left=i, width=fr, color=color)
            color = IONBLUE
            i += fr

        if int(frac[0]) >= 10:
            ax.text(float(frac[0]) / 200,
                    0.5,
                    labels[0],
                    transform=ax.transAxes,
                    fontsize=10,
                    verticalalignment='center',
                    horizontalalignment='center')
        if int(frac[1]) >= 10:
            ax.text((100 - float(frac[0])) / 200 + float(frac[0]) / 100,
                    0.5,
                    labels[1],
                    transform=ax.transAxes,
                    fontsize=10,
                    verticalalignment='center',
                    horizontalalignment='center')
        ax.set_title(title)
        ax.set_xlabel('% Capacity')

        canvas = FigureCanvas(fig)
        response = http.HttpResponse(content_type='image/png')
        canvas.print_png(response)
        return response
    except Exception as inst:
        open('/tmp/graphProblemLog.txt', 'w').write('problem: %s\n%s' % (inst, bk))


def residence_time(request):
    '''Attempts to estimate how long a file will remain on
    any given fileserver.  It looks at archived and deleted runs
    individually and creates separate bars for each of them.  The
    method used to calculate residence time is to look at the
    average time the last 20 were on the file server before being
    archived or deleted.  A simple difference in time is used. '''
    fileservers = models.FileServer.objects.all()
    numGraphs = math.ceil(math.sqrt(fileservers.count()))
    # create figure
    figwidth = 4 + (fileservers.count() - 1)   # inches
    figheight = 3   # inches
    numGraphs = math.ceil(math.sqrt(fileservers.count()))
    matplotlib.rcParams['font.size'] = 10.0 - math.sqrt(float(numGraphs))
    matplotlib.rcParams['axes.titlesize'] = 14.0 - math.sqrt(float(numGraphs))
    matplotlib.rcParams['xtick.labelsize'] = 10.0 - math.sqrt(float(numGraphs))
    matplotlib.rcParams['legend.fontsize'] = 10.0 - math.sqrt(float(numGraphs))
    fig = Figure(figsize=(figwidth, figheight))
    fig.patch.set_alpha(0)
    fig.subplots_adjust(left=0.01, right=0.99)
    ax = fig.add_subplot(1, 1, 1)
    count = 0
    max_scale = 0
    xticknames = []
    xtickpositions = []
    arch = models.Backup.objects.all().order_by('-backupDate')
    for n, fs in enumerate(fileservers):
        last_archived = arch.filter(experiment__expDir__icontains=fs.filesPrefix).filter(isBackedUp=True)[:20]
        last_deleted = arch.filter(experiment__expDir__icontains=fs.filesPrefix).filter(isBackedUp=False)[:20]
        bars = []
        time_before_arch = [(la.backupDate - la.experiment.date) for la in last_archived]
        time_before_delete = [(ld.backupDate - ld.experiment.date) for ld in last_deleted]
        if time_before_arch:
            ave_a = float(sum([d.days for d in time_before_arch])) / float(len(time_before_arch))
            bars.append(ave_a)
        else:
            bars.append(0.1)
        if time_before_delete:
            ave_d = float(sum([d.days for d in time_before_delete])) / float(len(time_before_delete))
            bars.append(ave_d)
        else:
            bars.append(0.1)
        if not bars:
            continue
        xticklabels = ['Archive', 'Delete']
        width = .2
        basepos = [count, count + width + .03]
        middlepos = float(sum(basepos)) / 2.0
        color = (IONYELLOW, IONBLUE)
        value = []
        for b in bars:
            if b != 0.1:
                value.append('%.f' % b)
            else:
                value.append('No Data')
        for i, b in enumerate(bars):
            ax.bar(basepos[i], bars[i], align='center', width=width, label=xticklabels[i], color=color[i])
            ax.text(basepos[i], bars[i], str(value[i]), horizontalalignment='center')
        ax.set_title('Average Days on Fileserver')
        xticknames.append(str(fs.filesPrefix))
        xtickpositions.append(middlepos)
        ax.yaxis.set_ticks_position("none")
        ax.set_yticklabels('')
        if max(bars) > max_scale:
            max_scale = max(bars)
        count += 1
    ax.set_xticklabels(xticknames)
    ax.set_xticks(xtickpositions)
    ax.legend(xticklabels)
    ax.set_ybound(0, max_scale * 1.50)
    canvas = FigureCanvas(fig)
    response = http.HttpResponse(content_type='image/png')
    canvas.print_png(response)
    return response
